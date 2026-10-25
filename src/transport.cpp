#include "nativecall/transport.hpp"

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nativecall {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Json = nlohmann::json;

std::uint16_t read16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((unsigned(p[0]) << 8) | p[1]);
}
std::uint32_t read32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
void write32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
}
void write16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
}
std::uint32_t randomSsrc() {
    static std::mutex mutex;
    static std::mt19937 generator(std::random_device{}());
    std::lock_guard lock(mutex);
    return std::max<std::uint32_t>(1, generator());
}

void validateIceServer(const IceServer& entry) {
    if (entry.url.empty() || entry.url.size() > 2048 || entry.url.find('@') != std::string::npos)
        throw std::invalid_argument("URL ICE inválida. Informe credenciais TURN nos campos username/password.");
    std::optional<rtc::IceServer> parsed;
    try { parsed.emplace(entry.url); }
    catch (...) {
        // libdatachannel includes the original URL in several parse errors;
        // keep the UI error useful without echoing embedded credentials.
        throw std::invalid_argument("Servidor ICE inválido. Verifique protocolo, endereço e porta.");
    }
#if !defined(NATIVE_CALL_USE_NICE) || !NATIVE_CALL_USE_NICE
    if (parsed->type == rtc::IceServer::Type::Turn && parsed->relayType != rtc::IceServer::RelayType::TurnUdp)
        throw std::invalid_argument("Esta compilação usa libjuice e oferece somente TURN/UDP. "
                                    "Use turn:servidor:3478?transport=udp; TURN/TCP ou TLS requer "
                                    "compilar libdatachannel com USE_NICE=ON e NATIVE_CALL_USE_NICE=ON.");
#endif
}

// Extend serial numbers without moving the greatest observed value backwards.
// Both RFC3550 sequence numbers and timestamps can wrap during a long call.
template<unsigned Bits> class SerialClock {
    bool initialized_{};
    std::int64_t greatest_{};
public:
    std::int64_t extend(std::uint32_t value) {
        constexpr auto mask = (std::uint64_t{1} << Bits) - 1;
        constexpr auto half = std::uint64_t{1} << (Bits - 1);
        if (!initialized_) {
            initialized_ = true;
            greatest_ = value;
            return greatest_;
        }
        auto delta = (std::uint64_t(value) - (std::uint64_t(greatest_) & mask)) & mask;
        auto signedDelta = delta >= half ? std::int64_t(delta) - std::int64_t(mask + 1)
                                        : std::int64_t(delta);
        auto result = greatest_ + signedDelta;
        greatest_ = std::max(greatest_, result);
        return result;
    }
};

struct RtpPayload {
    std::uint16_t sequence{};
    std::uint32_t timestamp{}, ssrc{};
    bool marker{};
    std::span<const std::uint8_t> bytes;
};
std::optional<RtpPayload> parseRtp(std::span<const std::uint8_t> packet) {
    if (packet.size() < 12 || packet.size() > 65536 || (packet[0] >> 6) != 2)
        return {};
    std::size_t header = 12 + (packet[0] & 15) * 4;
    if (header > packet.size()) return {};
    if (packet[0] & 16) {
        if (header + 4 > packet.size()) return {};
        header += 4 + std::size_t(read16(packet.data() + header + 2)) * 4;
        if (header > packet.size()) return {};
    }
    std::size_t padding = packet[0] & 32 ? packet.back() : 0;
    if ((packet[0] & 32) && (!padding || padding > packet.size() - header)) return {};
    if (header + padding >= packet.size()) return {};
    return RtpPayload{read16(packet.data() + 2), read32(packet.data() + 4),
                      read32(packet.data() + 8), bool(packet[1] & 128),
                      packet.subspan(header, packet.size() - header - padding)};
}

// Outgoing RTP packetization and incoming depacketization are independent.
// In particular, received audio never enters an outgoing media handler.
class MediaEndpoint final : public rtc::MediaHandler {
public:
    struct Events {
        std::function<void(std::vector<std::uint8_t>, std::uint64_t, bool)> frame;
        std::function<void()> keyframe;
        std::function<void(std::uint32_t)> bitrate;
    };
    const std::shared_ptr<rtc::RtpPacketizationConfig> config;
    const std::shared_ptr<rtc::RtcpSrReporter> reports;
private:
    bool video_;
    Events events_;
    std::shared_ptr<rtc::MediaHandler> packetizer_;
    std::shared_ptr<rtc::RtcpNackResponder> nack_;
    std::mutex receiveMutex_;
    SerialClock<32> time_;
    SerialClock<16> sequence_;
    std::set<std::int64_t> audioSeen_;
    std::uint32_t remoteSsrc_{};
    rtc::message_callback reply_;
    Clock::time_point lastPli_{}, lastFeedback_{};
    bool waitingKeyframe_{true};
    std::optional<std::int64_t> expectedSequence_;
    struct Assembly {
        std::int64_t timestamp{};
        std::map<std::int64_t, std::vector<std::uint8_t>> packets;
        std::optional<std::int64_t> end;
        Clock::time_point firstAt;
        Clock::time_point lastNack{};
        std::size_t bytes{};
    };
    std::map<std::int64_t, Assembly> frames_;
    unsigned lostFrames_{}, receivedFrames_{};

    void requestMissing(Assembly& frame, std::int64_t first, Clock::time_point now) {
        if (!reply_ || !frame.end || first > *frame.end || *frame.end - first > 4096 ||
            now - frame.firstAt < 8ms || now - frame.lastNack < 20ms) return;
        std::vector<std::uint16_t> missing;
        for (auto sequence = first; sequence <= *frame.end && missing.size() < 64; ++sequence)
            if (!frame.packets.contains(sequence)) missing.push_back(static_cast<std::uint16_t>(sequence));
        if (missing.empty()) return;
        frame.lastNack = now;
        // One PID per FCI keeps this request straightforward across a sequence
        // wrap; the count is strictly bounded and the NACK responder caches512
        // recent packets. Subsequent attempts stop when the80ms frame deadline
        // expires, so retransmission never grows a latency backlog.
        std::vector<std::uint8_t> packet(12 + missing.size() * 4);
        packet[0] = 0x81; packet[1] = 205;
        write16(packet.data() + 2, static_cast<std::uint16_t>(packet.size() / 4 - 1));
        write32(packet.data() + 4, config->ssrc);
        write32(packet.data() + 8, remoteSsrc_);
        for (std::size_t i = 0; i < missing.size(); ++i) write16(packet.data() + 12 + i * 4, missing[i]);
        reply_(rtc::make_message(reinterpret_cast<const rtc::byte*>(packet.data()),
                                reinterpret_cast<const rtc::byte*>(packet.data() + packet.size()),
                                rtc::Message::Control));
    }

    void sendPli(Clock::time_point now) {
        if (!reply_ || !remoteSsrc_ || now - lastPli_ < 500ms) return;
        lastPli_ = now;
        std::array<std::uint8_t, 12> packet{0x81, 206, 0, 2};
        write32(packet.data() + 4, config->ssrc);
        write32(packet.data() + 8, remoteSsrc_);
        reply_(rtc::make_message(reinterpret_cast<const rtc::byte*>(packet.data()),
                                reinterpret_cast<const rtc::byte*>(packet.data() + packet.size()),
                                rtc::Message::Control));
    }
    void sendFeedback(Clock::time_point now) {
        if (!reply_ || !remoteSsrc_ || now - lastFeedback_ < 1s) return;
        lastFeedback_ = now;
        unsigned total = lostFrames_ + receivedFrames_;
        unsigned lossPercent = total ? 100 * lostFrames_ / total : 0;
        std::uint32_t bitrate = lossPercent > 10 ? 500000 : lossPercent > 3 ? 1500000 : 12000000;
        lostFrames_ = receivedFrames_ = 0;
        std::array<std::uint8_t, 24> packet{0x8f, 206, 0, 5};
        write32(packet.data() + 4, config->ssrc);
        packet[12] = 'R'; packet[13] = 'E'; packet[14] = 'M'; packet[15] = 'B';
        packet[16] = 1;
        unsigned exponent{};
        while (bitrate > 0x3ffff) { bitrate >>= 1; ++exponent; }
        packet[17] = static_cast<std::uint8_t>((exponent << 2) | (bitrate >> 16));
        packet[18] = static_cast<std::uint8_t>(bitrate >> 8);
        packet[19] = static_cast<std::uint8_t>(bitrate);
        write32(packet.data() + 20, remoteSsrc_);
        reply_(rtc::make_message(reinterpret_cast<const rtc::byte*>(packet.data()),
                                reinterpret_cast<const rtc::byte*>(packet.data() + packet.size()),
                                rtc::Message::Control));
    }
    void control(const rtc::Message& message) {
        auto p = reinterpret_cast<const std::uint8_t*>(message.data());
        std::size_t offset{};
        while (offset + 4 <= message.size()) {
            auto size = (std::size_t(read16(p + offset + 2)) + 1) * 4;
            if (size < 4 || size > message.size() - offset || (p[offset] >> 6) != 2) break;
            auto* block = p + offset;
            unsigned fmt = block[0] & 31;
            if (video_ && block[1] == 206 && (fmt == 1 || fmt == 4) && size >= 12) {
                if (events_.keyframe) events_.keyframe();
            } else if (video_ && block[1] == 206 && fmt == 15 && size >= 24 &&
                       block[12] == 'R' && block[13] == 'E' && block[14] == 'M' && block[15] == 'B') {
                unsigned exponent = block[17] >> 2;
                std::uint64_t mantissa = ((block[17] & 3u) << 16) | (unsigned(block[18]) << 8) | block[19];
                auto bitrate = exponent > 32 ? std::uint64_t(12000000) : mantissa << exponent;
                if (events_.bitrate)
                    events_.bitrate(static_cast<std::uint32_t>(std::clamp<std::uint64_t>(bitrate, 250000, 12000000)));
            }
            offset += size;
        }
    }
    static bool assemble(const Assembly& frame, std::int64_t first,
                         std::vector<std::uint8_t>& output, bool& keyframe) {
        if (!frame.end || frame.packets.empty() || first > *frame.end ||
            *frame.end - first + 1 != std::int64_t(frame.packets.size())) return false;
        bool fragmentOpen{};
        bool sps{}, pps{}, idr{};
        auto appendStart = [&] { output.insert(output.end(), {0, 0, 0, 1}); };
        auto observe = [&](unsigned type) { sps |= type == 7; pps |= type == 8; idr |= type == 5; };
        auto expected = first;
        for (const auto& [sequence, payload] : frame.packets) {
            if (sequence != expected++ || payload.empty()) return false;
            unsigned type = payload[0] & 31;
            if (type > 0 && type < 24) {
                if (fragmentOpen) return false;
                observe(type); appendStart();
                output.insert(output.end(), payload.begin(), payload.end());
            } else if (type == 28) { // FU-A
                if (payload.size() < 3) return false;
                bool start = payload[1] & 128, end = payload[1] & 64;
                if (start) {
                    if (fragmentOpen || end) return false;
                    observe(payload[1] & 31); appendStart();
                    output.push_back(static_cast<std::uint8_t>((payload[0] & 0xe0) | (payload[1] & 31)));
                    fragmentOpen = true;
                } else if (!fragmentOpen) return false;
                output.insert(output.end(), payload.begin() + 2, payload.end());
                if (end) fragmentOpen = false;
            } else if (type == 24) { // STAP-A
                if (fragmentOpen) return false;
                std::size_t offset = 1;
                while (offset + 2 <= payload.size()) {
                    std::size_t length = read16(payload.data() + offset);
                    offset += 2;
                    if (!length || length > payload.size() - offset) return false;
                    observe(payload[offset] & 31); appendStart();
                    output.insert(output.end(), payload.begin() + offset, payload.begin() + offset + length);
                    offset += length;
                }
                if (offset != payload.size()) return false;
            } else return false;
        }
        keyframe = idr && sps && pps;
        return !fragmentOpen && !output.empty();
    }
    void drain(Clock::time_point now) {
        while (!frames_.empty()) {
            auto it = frames_.begin();
            auto& frame = it->second;
            bool aged = now - frame.firstAt >= 80ms;
            bool forced = frames_.size() > 4;
            // With no earlier frame boundary, wait briefly for reordered SPS/PPS.
            bool initialReady = expectedSequence_.has_value() || now - frame.firstAt >= 20ms;
            std::vector<std::uint8_t> bytes;
            bool keyframe{};
            auto first = expectedSequence_.value_or(frame.packets.begin()->first);
            bool complete = initialReady && assemble(frame, first, bytes, keyframe);
            // A complete independent IDR can recover after an entirely lost
            // preceding frame (whose boundary was consequently unknown).
            if (!complete && initialReady && first != frame.packets.begin()->first) {
                bytes.clear();
                bool independent{};
                if (assemble(frame, frame.packets.begin()->first, bytes, independent) && independent) {
                    complete = true; keyframe = true;
                }
            }
            if (!complete && !aged && !forced) {
                requestMissing(frame, first, now);
                break;
            }
            if (frame.end) expectedSequence_ = *frame.end + 1;
            else expectedSequence_.reset();
            auto timestamp = frame.timestamp;
            frames_.erase(it);
            if (!complete) {
                ++lostFrames_; waitingKeyframe_ = true; sendPli(now);
                continue;
            }
            ++receivedFrames_;
            if (keyframe) waitingKeyframe_ = false;
            if (waitingKeyframe_) { sendPli(now); continue; }
            if (events_.frame)
                events_.frame(std::move(bytes), std::uint64_t(std::max<std::int64_t>(0, timestamp)) * 1000000 / 90000,
                              keyframe);
        }
        sendFeedback(now);
    }
public:
    MediaEndpoint(bool video, std::uint32_t ssrc, std::uint8_t payloadType, Events events)
        : config(std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "nativecall", payloadType,
                                                              video ? 90000 : 48000)),
          reports(std::make_shared<rtc::RtcpSrReporter>(config)),
          video_(video), events_(std::move(events)), nack_(std::make_shared<rtc::RtcpNackResponder>(512)) {
        // One common sender epoch across mic, shared audio and video.  The
        // actual initial sequence numbers/SSRCs remain independently random.
        config->startTimestamp = 0;
        config->timestamp = 0;
        if (video)
            packetizer_ = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, config, 1150);
        else packetizer_ = std::make_shared<rtc::OpusRtpPacketizer>(config);
        packetizer_->addToChain(reports);
        packetizer_->addToChain(nack_);
    }
    void media(const rtc::Description::Media& desc) override { packetizer_->mediaChain(desc); }
    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override {
        packetizer_->outgoingChain(messages, send);
    }
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        std::lock_guard lock(receiveMutex_);
        reply_ = send;
        // NACKs concern our outgoing packets and are handled before consuming
        // RTCP. The receiver never passes decoded data to that outgoing path.
        nack_->incoming(messages, send);
        auto now = Clock::now();
        for (const auto& message : messages) {
            if (message->type == rtc::Message::Control) { control(*message); continue; }
            if (message->type != rtc::Message::Binary) continue;
            auto parsed = parseRtp({reinterpret_cast<const std::uint8_t*>(message->data()), message->size()});
            if (!parsed) continue;
            remoteSsrc_ = parsed->ssrc;
            auto seq = sequence_.extend(parsed->sequence);
            auto timestamp = time_.extend(parsed->timestamp);
            if (!video_) {
                if (parsed->bytes.size() > 1275 || !audioSeen_.insert(seq).second) continue;
                while (audioSeen_.size() > 128) audioSeen_.erase(audioSeen_.begin());
                if (events_.frame)
                    events_.frame({parsed->bytes.begin(), parsed->bytes.end()},
                                  std::uint64_t(std::max<std::int64_t>(0, timestamp)) * 1000000 / 48000, false);
                continue;
            }
            if (expectedSequence_ && seq < *expectedSequence_) continue;
            auto [it, inserted] = frames_.try_emplace(timestamp);
            auto& frame = it->second;
            if (inserted) { frame.timestamp = timestamp; frame.firstAt = now; }
            if (frame.packets.size() >= 4096 || frame.bytes + parsed->bytes.size() > 4 * 1024 * 1024) {
                frames_.erase(it); waitingKeyframe_ = true; expectedSequence_.reset(); sendPli(now); continue;
            }
            auto [packet, added] = frame.packets.try_emplace(seq, parsed->bytes.begin(), parsed->bytes.end());
            (void)packet;
            if (added) frame.bytes += parsed->bytes.size();
            if (parsed->marker) frame.end = seq;
        }
        messages.clear(); // no unbounded Track receive queue
        if (video_) drain(now);
    }
    void poll() {
        if (!video_) return;
        std::lock_guard lock(receiveMutex_);
        try { drain(Clock::now()); } catch (...) { /* peer may be closing */ }
    }
    bool requestKeyframe(const rtc::message_callback& send) override {
        if (!video_) return false;
        std::lock_guard lock(receiveMutex_);
        reply_ = send;
        waitingKeyframe_ = true;
        sendPli(Clock::now());
        return true;
    }
};
} // namespace

struct RtcTransport::Impl : std::enable_shared_from_this<RtcTransport::Impl> {
    struct AudioItem {
        std::string peer;
        AudioKind kind{};
        std::vector<std::uint8_t> bytes;
        std::uint64_t ptsUs{};
        std::uint64_t generation{};
    };
    struct Peer {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::array<std::shared_ptr<rtc::Track>, 3> tracks;
        std::array<std::shared_ptr<MediaEndpoint>, 3> endpoints;
        std::uint64_t generation{};
        Clock::time_point failedAt{};
        unsigned restarts{};
        std::uint32_t bitrate{12000000};
    };
    Callbacks callbacks;
    Config settings;
    std::atomic<bool> running{};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> controlQueue;
    std::deque<AudioItem> outgoingAudio, incomingAudio;
    std::deque<EncodedVideo> outgoingVideo;
    bool outgoingNeedsKeyframe{};
    struct IncomingVideo { EncodedVideo frame; std::uint64_t generation{}; };
    std::map<std::string, std::deque<IncomingVideo>> incomingVideo;
    std::set<std::string> incomingNeedsKeyframe;
    std::shared_ptr<rtc::WebSocket> socket;
    std::map<std::string, Peer> peers;
    std::map<std::string, std::string> names;
    std::map<std::string, std::vector<rtc::Candidate>> earlyCandidates;
    std::string selfId, sharedSource;
    bool sharing{}, welcomed{}, fatalSignaling{}, terminalReported{};
    unsigned retryCount{};
    std::uint64_t socketGeneration{}, peerGeneration{}, epochUs{};
    Clock::time_point nextConnect{}, lastPing{}, lastBitrateHint{}, lastKeyframeHint{};
    std::uint32_t lastSuggested{12000000};
    std::atomic<unsigned> droppedVideo{};

    explicit Impl(Callbacks c) : callbacks(std::move(c)) {}
    ~Impl() { stop(); }
    void state(std::string value) {
        if (callbacks.state) callbacks.state(std::move(value));
    }
    void post(std::function<void()> task) {
        std::lock_guard lock(mutex);
        if (!running || controlQueue.size() >= 1024) return;
        controlQueue.push_back(std::move(task));
        condition.notify_one();
    }
    void start(Config config) {
        stop();
        if ((config.signalingUrl.rfind("ws://", 0) != 0 && config.signalingUrl.rfind("wss://", 0) != 0) ||
            config.signalingUrl.size() > 2048 || config.room.empty() || config.room.size() > 64 ||
            config.displayName.empty() || config.displayName.size() > 64 ||
            config.accessToken.size() < 24 || config.accessToken.size() > 256)
            throw std::invalid_argument("URL, sala, nome ou token inválido (token: 24 a 256 bytes).");
        // Parse ICE URLs now so bad TURN configuration is reported to the caller.
        for (const auto& server : config.iceServers) validateIceServer(server);
        settings = std::move(config);
        epochUs = mediaNowUs();
        running = true;
        retryCount = 0; fatalSignaling = false; terminalReported = false;
        lastSuggested = 12000000; droppedVideo = 0; nextConnect = Clock::now();
        worker = std::thread([self = shared_from_this()] { self->run(); });
    }
    void stop() {
        running = false;
        condition.notify_all();
        if (worker.joinable()) {
            if (worker.get_id() == std::this_thread::get_id())
                std::terminate(); // callbacks must not destroy their own transport
            worker.join();
        }
        std::lock_guard lock(mutex);
        controlQueue.clear(); outgoingAudio.clear(); incomingAudio.clear();
        outgoingVideo.clear(); outgoingNeedsKeyframe = false; incomingVideo.clear();
        incomingNeedsKeyframe.clear();
    }
    void closePeer(Peer& peer) {
        peer.pc->resetCallbacks();
        for (auto& track : peer.tracks) if (track) { track->resetCallbacks(); track->close(); }
        peer.pc->close();
    }
    void clearPeers() {
        for (auto& [id, peer] : peers) { (void)id; closePeer(peer); }
        peers.clear(); earlyCandidates.clear();
        for (const auto& [id, name] : names) { (void)name; if (callbacks.peerLeft) callbacks.peerLeft(id); }
        names.clear();
    }
    void signal(Json value) {
        if (socket && socket->isOpen()) socket->send(value.dump());
    }
    void terminal(std::string reason) {
        fatalSignaling = true;
        state(reason);
        if (!terminalReported) {
            terminalReported = true;
            if (callbacks.stopped) callbacks.stopped(std::move(reason));
        }
    }
    void disconnected(std::uint64_t generation) {
        if (generation != socketGeneration || !running) return;
        ++socketGeneration;
        welcomed = false;
        clearPeers();
        if (socket) { socket->resetCallbacks(); socket->forceClose(); socket.reset(); }
        if (fatalSignaling) return;
        if (++retryCount > 8) {
            terminal("Reconexão esgotada. Saia e entre na chamada para tentar novamente.");
            return;
        }
        auto delay = std::min(30u, 1u << std::min(5u, retryCount - 1));
        nextConnect = Clock::now() + std::chrono::seconds(delay);
        state("Sinalização desconectada; reconectando em " + std::to_string(delay) + " s.");
    }
    void connect() {
        auto generation = ++socketGeneration;
        rtc::WebSocketConfiguration config;
        config.connectionTimeout = 8s;
        config.pingInterval = 10s;
        config.maxOutstandingPings = 2;
        config.maxMessageSize = 128 * 1024;
        socket = std::make_shared<rtc::WebSocket>(config);
        auto weak = weak_from_this();
        socket->onOpen([weak, generation] {
            if (auto self = weak.lock()) self->post([self, generation] {
                if (generation != self->socketGeneration) return;
                self->signal({{"type", "join"}, {"room", self->settings.room},
                              {"name", self->settings.displayName}, {"token", self->settings.accessToken}});
                self->lastPing = Clock::now();
                self->state("Autenticando na sala...");
            });
        });
        socket->onClosed([weak, generation] {
            if (auto self = weak.lock()) self->post([self, generation] { self->disconnected(generation); });
        });
        socket->onError([weak, generation](std::string) {
            // Library error strings may include URLs; never echo credentials.
            if (auto self = weak.lock()) self->post([self, generation] { self->disconnected(generation); });
        });
        socket->onMessage(nullptr, [weak, generation](std::string message) {
            if (message.size() > 128 * 1024) return;
            if (auto self = weak.lock()) self->post([self, generation, message = std::move(message)] {
                if (generation == self->socketGeneration) self->message(message);
            });
        });
        state("Conectando ao servidor...");
        nextConnect = Clock::time_point::max();
        try { socket->open(settings.signalingUrl); }
        catch (...) { disconnected(generation); }
    }
    void suggestBitrate(std::uint32_t value) {
        if (callbacks.bitrateSuggested && value != lastSuggested) {
            lastSuggested = value;
            callbacks.bitrateSuggested(value);
        }
    }
    void keyframe() {
        if (Clock::now() - lastKeyframeHint < 300ms) return;
        lastKeyframeHint = Clock::now();
        if (callbacks.keyframeRequested) callbacks.keyframeRequested();
    }
    bool currentPeer(const std::string& id, std::uint64_t generation) const {
        auto it = peers.find(id);
        return it != peers.end() && it->second.generation == generation;
    }
    Peer& makePeer(const std::string& id, bool offer) {
        if (auto it = peers.find(id); it != peers.end()) {
            closePeer(it->second); peers.erase(it);
        }
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.forceMediaTransport = true;
        config.mtu = 1280;
        for (const auto& entry : settings.iceServers) {
            rtc::IceServer ice(entry.url);
            if (ice.type == rtc::IceServer::Type::Turn) { ice.username = entry.username; ice.password = entry.password; }
            config.iceServers.push_back(std::move(ice));
        }
        Peer peer;
        peer.pc = std::make_shared<rtc::PeerConnection>(config);
        peer.generation = ++peerGeneration;
        auto generation = peer.generation;
        auto weak = weak_from_this();
        peer.pc->onLocalDescription([weak, id, generation](rtc::Description desc) {
            if (auto self = weak.lock()) self->post([self, id, generation, desc = std::move(desc)] {
                if (!self->currentPeer(id, generation)) return;
                self->signal({{"type", "description"}, {"to", id}, {"sdp", std::string(desc)},
                              {"descriptionType", desc.typeString()}});
            });
        });
        peer.pc->onLocalCandidate([weak, id, generation](rtc::Candidate candidate) {
            if (auto self = weak.lock()) self->post([self, id, generation, candidate = std::move(candidate)] {
                if (!self->currentPeer(id, generation)) return;
                self->signal({{"type", "candidate"}, {"to", id}, {"candidate", std::string(candidate)},
                              {"mid", candidate.mid()}});
            });
        });
        peer.pc->onStateChange([weak, id, generation](rtc::PeerConnection::State state) {
            if (auto self = weak.lock()) self->post([self, id, generation, state] {
                if (!self->currentPeer(id, generation)) return;
                auto& peer = self->peers.at(id);
                if (state == rtc::PeerConnection::State::Connected) {
                    peer.failedAt = {}; peer.restarts = 0;
                    self->state("Conectado com " + self->names[id]);
                    self->keyframe();
                    self->signal({{"type", "share"}, {"active", self->sharing}, {"source", self->sharedSource}});
                } else if (state == rtc::PeerConnection::State::Disconnected ||
                           state == rtc::PeerConnection::State::Failed) {
                    peer.failedAt = Clock::now();
                    self->state("Conexão com " + self->names[id] + " interrompida; tentando recuperar.");
                }
            });
        });
        for (unsigned index = 0; index < 3; ++index) {
            bool video = index == 2;
            const std::string mid = index == 0 ? "microphone" : index == 1 ? "shared-audio" : "shared-video";
            auto ssrc = randomSsrc();
            std::uint8_t payloadType = video ? 102 : index == 0 ? 111 : 112;
            rtc::Description::Media media = video
                ? rtc::Description::Media(rtc::Description::Video(mid, rtc::Description::Direction::SendRecv))
                : rtc::Description::Media(rtc::Description::Audio(mid, rtc::Description::Direction::SendRecv));
            if (video) {
                rtc::Description::Video desc(mid, rtc::Description::Direction::SendRecv);
                desc.addH264Codec(payloadType, "profile-level-id=42e02a;packetization-mode=1;level-asymmetry-allowed=1");
                desc.addSSRC(ssrc, "nativecall", "share", "video");
                media = desc;
            } else {
                rtc::Description::Audio desc(mid, rtc::Description::Direction::SendRecv);
                desc.addOpusCodec(payloadType);
                desc.addSSRC(ssrc, "nativecall", index == 0 ? "voice" : "share", mid);
                media = desc;
            }
            auto track = peer.pc->addTrack(media);
            MediaEndpoint::Events events;
            events.frame = [weak, id, generation, index](std::vector<std::uint8_t> bytes, std::uint64_t pts, bool key) {
                if (auto self = weak.lock()) {
                    std::lock_guard lock(self->mutex);
                    if (!self->running) return;
                    if (index == 2) {
                        // Preserve H264 dependencies inside a small bounded
                        // queue. If it overflows, discard the dependency chain
                        // and wait for an IDR; replacing arbitrary P frames
                        // would corrupt the subsequent decoded picture.
                        if (!key && self->incomingNeedsKeyframe.contains(id)) return;
                        if (key) self->incomingNeedsKeyframe.erase(id);
                        auto& queue = self->incomingVideo[id];
                        if (queue.size() >= 8) {
                            queue.clear();
                            self->incomingNeedsKeyframe.insert(id);
                            if (self->controlQueue.size() < 1024)
                                self->controlQueue.push_back([self, id, generation] {
                                    if (self->currentPeer(id, generation)) self->peers.at(id).tracks[2]->requestKeyframe();
                                });
                            if (!key) return;
                            self->incomingNeedsKeyframe.erase(id);
                        }
                        queue.push_back(IncomingVideo{EncodedVideo{std::move(bytes), pts, key}, generation});
                    } else {
                        if (self->incomingAudio.size() >= 64) self->incomingAudio.pop_front();
                        self->incomingAudio.push_back({id, index == 0 ? AudioKind::Microphone : AudioKind::Shared,
                                                       std::move(bytes), pts, generation});
                    }
                    self->condition.notify_one();
                }
            };
            events.keyframe = [weak] {
                if (auto self = weak.lock()) self->post([self] { self->keyframe(); });
            };
            events.bitrate = [weak, id, generation](std::uint32_t bitrate) {
                if (auto self = weak.lock()) self->post([self, id, generation, bitrate] {
                    if (!self->currentPeer(id, generation)) return;
                    self->peers.at(id).bitrate = bitrate;
                    std::uint32_t minimum = 12000000;
                    for (const auto& [peerId, peer] : self->peers) { (void)peerId; minimum = std::min(minimum, peer.bitrate); }
                    self->suggestBitrate(minimum);
                });
            };
            auto endpoint = std::make_shared<MediaEndpoint>(video, ssrc, payloadType, std::move(events));
            track->setMediaHandler(endpoint);
            peer.tracks[index] = std::move(track);
            peer.endpoints[index] = std::move(endpoint);
        }
        auto [it, inserted] = peers.emplace(id, std::move(peer));
        (void)inserted;
        if (offer) it->second.pc->setLocalDescription(rtc::Description::Type::Offer);
        return it->second;
    }
    void addName(const Json& entry) {
        auto id = entry.at("id").get<std::string>();
        auto name = entry.at("name").get<std::string>();
        if (id.empty() || id.size() > 128 || name.size() > 64 || id == selfId) return;
        bool added = names.try_emplace(id, name).second;
        if (added && callbacks.peerJoined) callbacks.peerJoined(id, name);
        if (entry.value("sharing", false) && callbacks.shareState)
            callbacks.shareState(id, entry.value("source", std::string{}), true);
    }
    void message(const std::string& wire) {
        auto value = Json::parse(wire, nullptr, false);
        if (!value.is_object()) return;
        try {
            auto type = value.at("type").get<std::string>();
            if (type == "welcome") {
                clearPeers();
                selfId = value.at("id").get<std::string>();
                const auto& remote = value.at("peers");
                if (!remote.is_array() || remote.size() > 3 || selfId.empty()) throw std::runtime_error("sala inválida");
                welcomed = true; retryCount = 0;
                state("Na chamada • " + std::to_string(remote.size() + 1) + "/4 participantes");
                for (const auto& entry : remote) { addName(entry); makePeer(entry.at("id").get<std::string>(), true); }
                signal({{"type", "share"}, {"active", sharing}, {"source", sharedSource}});
            } else if (type == "peer-joined") {
                if (names.size() < 3) addName(value);
            } else if (type == "peer-left") {
                auto id = value.at("id").get<std::string>();
                if (auto it = peers.find(id); it != peers.end()) { closePeer(it->second); peers.erase(it); }
                earlyCandidates.erase(id);
                if (names.erase(id) && callbacks.peerLeft) callbacks.peerLeft(id);
            } else if (type == "description") {
                auto id = value.at("from").get<std::string>();
                if (!names.contains(id)) return;
                auto kind = value.at("descriptionType").get<std::string>();
                if (kind != "offer" && kind != "answer") return;
                auto sdp = value.at("sdp").get<std::string>();
                if (sdp.size() > 100 * 1024) return;
                if (kind == "offer") {
                    if (!peers.contains(id)) makePeer(id, false);
                    peers.at(id).pc->setRemoteDescription(rtc::Description(sdp, kind));
                    peers.at(id).pc->setLocalDescription(rtc::Description::Type::Answer);
                } else if (peers.contains(id)) peers.at(id).pc->setRemoteDescription(rtc::Description(sdp, kind));
                if (auto pending = earlyCandidates.find(id); pending != earlyCandidates.end() && peers.contains(id)) {
                    for (auto& candidate : pending->second) peers.at(id).pc->addRemoteCandidate(std::move(candidate));
                    earlyCandidates.erase(pending);
                }
            } else if (type == "candidate") {
                auto id = value.at("from").get<std::string>();
                if (!names.contains(id)) return;
                auto candidate = value.at("candidate").get<std::string>();
                auto mid = value.at("mid").get<std::string>();
                if (candidate.size() > 4096 || mid.size() > 64) return;
                if (auto it = peers.find(id); it != peers.end() && it->second.pc->remoteDescription())
                    it->second.pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
                else if (earlyCandidates[id].size() < 64) earlyCandidates[id].emplace_back(candidate, mid);
            } else if (type == "reset") {
                auto id = value.at("from").get<std::string>();
                if (!names.contains(id)) return;
                if (auto it = peers.find(id); it != peers.end()) { closePeer(it->second); peers.erase(it); }
                earlyCandidates.erase(id);
                // The sender of reset is the elected offerer.
                if (callbacks.shareState) callbacks.shareState(id, {}, false);
            } else if (type == "share") {
                auto id = value.at("from").get<std::string>();
                if (names.contains(id) && callbacks.shareState)
                    callbacks.shareState(id, value.value("source", std::string{}), value.at("active").get<bool>());
            } else if (type == "error") {
                auto code = value.value("code", std::string{});
                auto reason = "Servidor: " + value.value("message", std::string{"erro de sinalização"});
                if (code == "unauthorized" || code == "room_full" || code == "invalid_join" || code == "bad_request")
                    terminal(std::move(reason));
                else state(std::move(reason));
            }
        } catch (const std::exception&) {
            state("Mensagem de sinalização inválida; verifique a versão do servidor.");
        }
    }
    void sendFrame(Peer& peer, unsigned index, std::span<const std::uint8_t> bytes, std::uint64_t ptsUs) {
        auto& track = peer.tracks[index];
        if (!track || !track->isOpen()) return;
        auto& endpoint = peer.endpoints[index];
        auto elapsed = ptsUs >= epochUs ? ptsUs - epochUs : 0;
        auto rate = index == 2 ? 90000ull : 48000ull;
        endpoint->config->timestamp = static_cast<std::uint32_t>((elapsed / 1000000) * rate +
                                                               (elapsed % 1000000) * rate / 1000000);
        if (endpoint->config->timestamp - endpoint->reports->lastReportedTimestamp() >= rate)
            endpoint->reports->setNeedsToReport();
        track->send(reinterpret_cast<const rtc::byte*>(bytes.data()), bytes.size());
    }
    void tick() {
        auto now = Clock::now();
        if (!fatalSignaling && nextConnect <= now) connect();
        if (welcomed && now - lastPing >= 25s) { signal({{"type", "ping"}}); lastPing = now; }
        std::vector<std::string> restart;
        for (auto& [id, peer] : peers) {
            for (auto& endpoint : peer.endpoints) endpoint->poll();
            // Only one side re-offers, eliminating offer glare during recovery.
            if (peer.failedAt != Clock::time_point{} && now - peer.failedAt > 3s && selfId < id && peer.restarts < 4)
                restart.push_back(id);
        }
        for (const auto& id : restart) {
            auto attempts = peers.at(id).restarts + 1;
            signal({{"type", "reset"}, {"to", id}});
            auto& peer = makePeer(id, true);
            peer.restarts = attempts;
            // A new connection that never gets Connected still needs a retry.
            peer.failedAt = now + std::chrono::seconds(2 * attempts);
        }
        if (droppedVideo.load() && now - lastBitrateHint > 1s) {
            droppedVideo.exchange(0); lastBitrateHint = now;
            suggestBitrate(std::max<std::uint32_t>(250000, lastSuggested * 3 / 4));
        }
    }
    void run() {
        while (running) {
            std::deque<std::function<void()>> tasks;
            std::deque<AudioItem> txAudio, rxAudio;
            std::optional<EncodedVideo> txVideo;
            std::map<std::string, std::deque<IncomingVideo>> rxVideo;
            {
                std::unique_lock lock(mutex);
                condition.wait_for(lock, 10ms, [&] {
                    return !running || !controlQueue.empty() || !outgoingAudio.empty() ||
                           !incomingAudio.empty() || !outgoingVideo.empty() || !incomingVideo.empty();
                });
                if (!running) break;
                for (unsigned i = 0; i < 32 && !controlQueue.empty(); ++i) {
                    tasks.push_back(std::move(controlQueue.front())); controlQueue.pop_front();
                }
                txAudio.swap(outgoingAudio); rxAudio.swap(incomingAudio);
                if (!outgoingVideo.empty()) {
                    txVideo = std::move(outgoingVideo.front()); outgoingVideo.pop_front();
                }
                rxVideo.swap(incomingVideo);
            }
            try {
                for (auto& task : tasks) task();
                // Speech is always processed ahead of screen video.
                for (const auto& audio : txAudio)
                    for (auto& [id, peer] : peers) { (void)id;
                        try { sendFrame(peer, audio.kind == AudioKind::Microphone ? 0 : 1, audio.bytes, audio.ptsUs); }
                        catch (...) { /* connection state callback handles recovery */ }
                    }
                for (auto& audio : rxAudio)
                    if (currentPeer(audio.peer, audio.generation) && callbacks.audio)
                        callbacks.audio(std::move(audio.peer), audio.kind, std::move(audio.bytes), audio.ptsUs);
                if (txVideo) for (auto& [id, peer] : peers) { (void)id;
                    try { sendFrame(peer, 2, txVideo->bytes, txVideo->ptsUs); }
                    catch (...) { /* do not block call audio on a failed viewer */ }
                }
                for (auto& [id, videos] : rxVideo)
                    for (auto& video : videos)
                        if (currentPeer(id, video.generation) && callbacks.video) callbacks.video(id, std::move(video.frame));
                tick();
            } catch (const std::exception&) { state("Falha de transporte; a conexão será recuperada quando possível."); }
        }
        if (socket) {
            try { if (socket->isOpen()) signal({{"type", "leave"}}); } catch (...) {}
            socket->resetCallbacks(); socket->forceClose(); socket.reset();
        }
        clearPeers();
        welcomed = false; selfId.clear(); sharing = false; sharedSource.clear();
    }
};

RtcTransport::RtcTransport(Callbacks callbacks) : impl_(std::make_shared<Impl>(std::move(callbacks))) {}
RtcTransport::~RtcTransport() { impl_->stop(); }
void RtcTransport::join(Config settings) { impl_->start(std::move(settings)); }
void RtcTransport::leave() { impl_->stop(); }
void RtcTransport::sendAudio(AudioKind kind, std::span<const std::uint8_t> opus, std::uint64_t ptsUs) {
    if (opus.empty() || opus.size() > 1275) return;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return;
    if (impl_->outgoingAudio.size() >= 20) impl_->outgoingAudio.pop_front();
    impl_->outgoingAudio.push_back({{}, kind, {opus.begin(), opus.end()}, ptsUs});
    impl_->condition.notify_one();
}
void RtcTransport::sendVideo(const EncodedVideo& frame) {
    if (frame.bytes.empty() || frame.bytes.size() > 4 * 1024 * 1024) return;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) return;
    if (impl_->outgoingNeedsKeyframe && !frame.keyframe) return;
    if (impl_->outgoingVideo.size() >= 4) {
        impl_->outgoingVideo.clear();
        impl_->outgoingNeedsKeyframe = true;
        ++impl_->droppedVideo;
        auto self = impl_;
        if (impl_->controlQueue.size() < 1024)
            impl_->controlQueue.push_back([self] { self->keyframe(); });
        if (!frame.keyframe) { impl_->condition.notify_one(); return; }
    }
    if (frame.keyframe) impl_->outgoingNeedsKeyframe = false;
    impl_->outgoingVideo.push_back(frame);
    impl_->condition.notify_one();
}
void RtcTransport::announceShare(std::string sourceName, bool active) {
    if (sourceName.size() > 512) sourceName.resize(512);
    auto self = impl_;
    self->post([self, sourceName = std::move(sourceName), active] {
        self->sharing = active; self->sharedSource = sourceName;
        if (self->welcomed) self->signal({{"type", "share"}, {"active", active}, {"source", sourceName}});
        if (active) self->keyframe();
    });
}
void RtcTransport::requestRemoteKeyframe(std::string peerId) {
    auto self = impl_;
    self->post([self, peerId = std::move(peerId)] {
        if (auto it = self->peers.find(peerId); it != self->peers.end())
            it->second.tracks[2]->requestKeyframe();
    });
}
} // namespace nativecall
