// Native Call signaling service. Media never passes through this WebSocket server.
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxMessageBytes = 128 * 1024;
constexpr std::size_t MaxClients = 128;
constexpr std::size_t MaxRooms = 32;
constexpr std::size_t MaxQueuedEvents = 512;
constexpr std::size_t MaxBufferedBytes = 1024 * 1024;
constexpr std::size_t MaxParticipants = 4;
volatile std::sig_atomic_t stopped = 0;

void signalHandler(int) { stopped = 1; }

bool sameToken(const std::string& a, const std::string& b) {
    // Both length and all compared bytes are incorporated without early exit.
    std::size_t difference = a.size() ^ b.size();
    const auto count = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto left = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const auto right = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        difference |= left ^ right;
    }
    return difference == 0;
}

bool validLabel(const std::string& value, std::size_t limit) {
    return !value.empty() && value.size() <= limit &&
           std::none_of(value.begin(), value.end(), [](unsigned char ch) {
               return ch < 0x20 || ch == 0x7f;
           });
}

std::string field(const json& object, const char* key, std::size_t maxLength,
                  bool allowEmpty = false) {
    const auto item = object.find(key);
    if (item == object.end() || !item->is_string())
        throw std::invalid_argument(std::string("Campo inválido: ") + key);
    auto value = item->get<std::string>();
    if (value.size() > maxLength || (!allowEmpty && value.empty()))
        throw std::invalid_argument(std::string("Tamanho inválido: ") + key);
    return value;
}

std::string newId() {
    // IDs are server-owned, not authentication credentials. Access uses the room token.
    static std::random_device entropy;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i)
        out << std::hex << std::setw(8) << std::setfill('0')
            << static_cast<std::uint32_t>(entropy());
    return out.str();
}

class Signaling final : public std::enable_shared_from_this<Signaling> {
    struct Client {
        explicit Client(std::shared_ptr<rtc::WebSocket> connection)
            : socket(std::move(connection)) {}
        std::shared_ptr<rtc::WebSocket> socket;
        std::string room;
        std::string name;
        std::string shareSource;
        bool sharing = false;
        Clock::time_point connected = Clock::now();
        Clock::time_point lastMessage = Clock::now();
        Clock::time_point refill = Clock::now();
        double tokens = 240;
    };
    enum class EventKind { Connected, Message, Closed };
    struct Event {
        EventKind kind;
        std::string id;
        std::shared_ptr<rtc::WebSocket> socket;
        std::string data;
    };

public:
    explicit Signaling(std::string token) : token_(std::move(token)) {}
    ~Signaling() { stop(); }

    void start() {
        worker_ = std::thread([this] { run(); });
    }

    void stop() {
        {
            std::lock_guard lock(queueMutex_);
            running_ = false;
        }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    void accept(std::shared_ptr<rtc::WebSocket> socket) {
        std::string id;
        try {
            id = newId();
        } catch (...) {
            socket->close();
            return;
        }
        const auto weak = weak_from_this();
        if (!enqueue({EventKind::Connected, id, socket, {}})) {
            socket->close();
            return;
        }
        socket->onMessage([weak, id, socketWeak = std::weak_ptr(socket)](rtc::message_variant data) {
            if (auto self = weak.lock()) {
                if (auto text = std::get_if<std::string>(&data);
                    text && text->size() <= MaxMessageBytes &&
                    self->enqueue({EventKind::Message, id, {}, std::move(*text)}))
                    return;
                if (auto ws = socketWeak.lock()) ws->close();
            }
        });
        socket->onClosed([weak, id] {
            if (auto self = weak.lock()) self->enqueue({EventKind::Closed, id, {}, {}});
        });
        socket->onError([weak, id](const std::string&) {
            if (auto self = weak.lock()) self->enqueue({EventKind::Closed, id, {}, {}});
        });
    }

private:
    bool enqueue(Event event) {
        {
            std::lock_guard lock(queueMutex_);
            if (!running_ || queue_.size() >= MaxQueuedEvents) return false;
            queue_.push_back(std::move(event));
        }
        wake_.notify_one();
        return true;
    }

    static void send(const std::shared_ptr<rtc::WebSocket>& socket, const json& message) {
        if (!socket || !socket->isOpen()) return;
        try {
            if (socket->bufferedAmount() > MaxBufferedBytes) {
                socket->close();
                return;
            }
            socket->send(message.dump());
        } catch (...) {
            try { socket->close(); } catch (...) {}
        }
    }

    static void reject(const std::shared_ptr<rtc::WebSocket>& socket,
                       const std::string& message, const std::string& code) {
        send(socket, { {"type", "error"}, {"message", message}, {"code", code} });
        try { socket->close(); } catch (...) {}
    }

    void broadcast(const std::string& room, const std::string& except, const json& message) {
        for (const auto& [id, client] : clients_)
            if (id != except && client.room == room) send(client.socket, message);
    }

    void remove(const std::string& id) {
        const auto found = clients_.find(id);
        if (found == clients_.end()) return;
        const auto room = found->second.room;
        auto socket = std::move(found->second.socket);
        clients_.erase(found);
        if (!room.empty()) broadcast(room, id, { {"type", "peer-left"}, {"id", id} });
        try { socket->close(); } catch (...) {}
    }

    void join(const std::string& id, Client& client, const json& message) {
        if (!client.room.empty()) throw std::invalid_argument("Você já entrou em uma sala.");
        const auto room = field(message, "room", 64);
        const auto name = field(message, "name", 64);
        const auto token = field(message, "token", 256);
        if (!sameToken(token, token_)) {
            reject(client.socket, "Acesso negado. Verifique o token de acesso.", "unauthorized");
            return;
        }
        if (!validLabel(room, 64) || !validLabel(name, 64))
            throw std::invalid_argument("Sala ou nome contém caracteres inválidos.");

        json peers = json::array();
        std::unordered_map<std::string, bool> rooms;
        for (const auto& [peerId, peer] : clients_) {
            if (peer.room.empty()) continue;
            rooms[peer.room] = true;
            if (peer.room == room)
                peers.push_back({ {"id", peerId}, {"name", peer.name},
                                  {"sharing", peer.sharing}, {"source", peer.shareSource} });
        }
        if (peers.size() >= MaxParticipants) {
            reject(client.socket, "A chamada já tem quatro participantes.", "room_full");
            return;
        }
        if (!rooms.contains(room) && rooms.size() >= MaxRooms) {
            reject(client.socket, "Servidor ocupado. Tente novamente mais tarde.", "server_full");
            return;
        }
        client.room = room;
        client.name = name;
        send(client.socket, { {"type", "welcome"}, {"id", id}, {"peers", peers} });
        broadcast(room, id, { {"type", "peer-joined"}, {"id", id}, {"name", name} });
    }

    void process(const std::string& id, const std::string& data) {
        const auto found = clients_.find(id);
        if (found == clients_.end()) return;
        auto& client = found->second;
        const auto now = Clock::now();
        client.tokens = std::min(240., client.tokens +
            std::chrono::duration<double>(now - client.refill).count() * 60.);
        client.refill = now;
        if (client.tokens < 1) {
            reject(client.socket, "Muitas mensagens de sinalização.", "rate_limited");
            return;
        }
        client.tokens -= 1;
        client.lastMessage = now;

        try {
            const auto message = json::parse(data,
                [](int depth, json::parse_event_t, json&) {
                    if (depth > 8) throw std::invalid_argument("JSON excessivamente aninhado.");
                    return true;
                });
            if (!message.is_object()) throw std::invalid_argument("Mensagem JSON inválida.");
            const auto type = field(message, "type", 24);
            if (type == "join") {
                join(id, client, message);
                return;
            }
            if (client.room.empty()) throw std::invalid_argument("Entre em uma sala primeiro.");
            if (type == "leave") {
                remove(id);
                return;
            }
            if (type == "ping") {
                send(client.socket, { {"type", "pong"} });
                return;
            }
            if (type == "share") {
                const auto active = message.find("active");
                if (active == message.end() || !active->is_boolean())
                    throw std::invalid_argument("Estado de transmissão inválido.");
                const auto source = field(message, "source", 512, true);
                if (std::any_of(source.begin(), source.end(), [](unsigned char ch) {
                    return ch < 0x20 || ch == 0x7f;
                })) throw std::invalid_argument("Nome da fonte inválido.");
                client.sharing = active->get<bool>();
                client.shareSource = client.sharing ? source : "";
                broadcast(client.room, id, { {"type", "share"}, {"from", id},
                          {"source", client.shareSource}, {"active", client.sharing} });
                return;
            }
            const auto to = field(message, "to", 32);
            const auto target = clients_.find(to);
            if (to == id || target == clients_.end() || target->second.room != client.room)
                throw std::invalid_argument("Participante de destino indisponível.");
            json forwarded{ {"type", type}, {"from", id}, {"to", to} };
            if (type == "description") {
                const auto descriptionType = field(message, "descriptionType", 8);
                if (descriptionType != "offer" && descriptionType != "answer")
                    throw std::invalid_argument("Tipo de SDP inválido.");
                forwarded["descriptionType"] = descriptionType;
                forwarded["sdp"] = field(message, "sdp", 120 * 1024);
            } else if (type == "candidate") {
                forwarded["candidate"] = field(message, "candidate", 4096, true);
                forwarded["mid"] = field(message, "mid", 64, true);
            } else if (type != "reset") {
                throw std::invalid_argument("Tipo de mensagem desconhecido.");
            }
            send(target->second.socket, forwarded);
        } catch (const json::exception&) {
            reject(client.socket, "Mensagem JSON inválida.", "bad_request");
        } catch (const std::invalid_argument& e) {
            reject(client.socket, e.what(), "bad_request");
        }
    }

    void maintain() {
        const auto now = Clock::now();
        std::vector<std::string> expired;
        for (const auto& [id, client] : clients_) {
            if (client.socket->isClosed() ||
                (client.room.empty() && now - client.connected > std::chrono::seconds(10)) ||
                now - client.lastMessage > std::chrono::seconds(120))
                expired.push_back(id);
        }
        for (const auto& id : expired) remove(id);
    }

    void run() {
        auto nextMaintenance = Clock::now();
        for (;;) {
            Event event;
            bool hasEvent = false;
            {
                std::unique_lock lock(queueMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !running_ || !queue_.empty();
                });
                if (!running_) break;
                if (!queue_.empty()) {
                    event = std::move(queue_.front());
                    queue_.pop_front();
                    hasEvent = true;
                }
            }
            try {
                if (hasEvent) {
                    if (event.kind == EventKind::Connected) {
                        if (clients_.size() >= MaxClients || clients_.contains(event.id))
                            reject(event.socket, "Servidor ocupado.", "server_full");
                        else clients_.emplace(event.id, Client{ std::move(event.socket) });
                    } else if (event.kind == EventKind::Closed) remove(event.id);
                    else process(event.id, event.data);
                }
                if (Clock::now() >= nextMaintenance) {
                    maintain();
                    nextMaintenance = Clock::now() + std::chrono::seconds(1);
                }
            } catch (const std::exception&) {
                // A malformed/closed connection cannot terminate service for other rooms.
                if (hasEvent) remove(event.id);
            }
        }
        for (auto& [id, client] : clients_) {
            client.socket->resetCallbacks();
            try { client.socket->close(); } catch (...) {}
        }
        clients_.clear();
        std::lock_guard lock(queueMutex_);
        queue_.clear();
    }

    const std::string token_;
    std::mutex queueMutex_;
    std::condition_variable wake_;
    std::deque<Event> queue_;
    bool running_ = true;
    std::thread worker_;
    // Only worker_ accesses room/client state. No WebSocket callback runs under a state lock.
    std::unordered_map<std::string, Client> clients_;
};

} // namespace

int main(int argc, char** argv) {
    try {
        std::string bind = "127.0.0.1";
        std::uint16_t port = 8080;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "native-call-signaling [--bind 127.0.0.1] [--port 8080]\n"
                             "Configure NATIVE_CALL_TOKEN (24 a 256 bytes) no ambiente.\n"
                             "Use um proxy HTTPS/WSS para conexões pela internet.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("Argumento sem valor: " + argument);
            const std::string value = argv[++i];
            if (argument == "--bind") bind = value;
            else if (argument == "--port") {
                std::size_t consumed = 0;
                const auto parsed = std::stoul(value, &consumed);
                if (consumed != value.size() || parsed < 1 || parsed > 65535)
                    throw std::invalid_argument("Porta inválida.");
                port = static_cast<std::uint16_t>(parsed);
            } else throw std::invalid_argument("Argumento desconhecido: " + argument);
        }
        const auto tokenEnvironment = std::getenv("NATIVE_CALL_TOKEN");
        const std::string token = tokenEnvironment ? tokenEnvironment : "";
        if (token.size() < 24 || token.size() > 256)
            throw std::invalid_argument("Defina NATIVE_CALL_TOKEN com 24 a 256 bytes.");
        rtc::InitLogger(rtc::LogLevel::Warning);
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        auto service = std::make_shared<Signaling>(token);
        service->start();
        rtc::WebSocketServer::Configuration config;
        config.bindAddress = bind;
        config.port = port;
        config.maxMessageSize = MaxMessageBytes;
        config.connectionTimeout = std::chrono::seconds(10);
        rtc::WebSocketServer server(config);
        server.onClient([weak = std::weak_ptr(service)](std::shared_ptr<rtc::WebSocket> socket) {
            if (auto live = weak.lock()) live->accept(std::move(socket));
            else socket->close();
        });
        std::cout << "Sinalização em ws://" << bind << ':' << server.port()
                  << " — máximo de quatro participantes por sala.\n";
        while (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        server.stop();
        service->stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Não foi possível iniciar a sinalização: " << e.what() << '\n';
        return 1;
    }
}
