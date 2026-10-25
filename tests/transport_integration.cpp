// Real libdatachannel/ICE/DTLS/SRTP mesh integration test. Payloads below are
// deterministic transport fixtures, never audio/video sources used by the app.
#include "nativecall/transport.hpp"

#include <rtc/rtc.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using nativecall::AudioKind;
using nativecall::EncodedVideo;
using nativecall::RtcTransport;

namespace {
std::vector<std::uint8_t> audioFixture(unsigned sender, AudioKind kind) {
    // The transport treats encoded Opus as opaque. This fixture verifies exact
    // bytes and source identity; the audio codec is tested separately on Windows.
    return {0xf8, 0xff, 0xfe, 0x4e, 0x43, static_cast<std::uint8_t>(sender),
            static_cast<std::uint8_t>(kind == AudioKind::Shared ? 2 : 1), 0xa5};
}

std::vector<std::uint8_t> videoFixture(unsigned sender) {
    // Three Annex-B NAL units, with an IDR large enough to require ~36 FU-A RTP
    // fragments. This is a packetization fixture and is not fed into a decoder.
    std::vector<std::uint8_t> data{0, 0, 0, 1, 0x67, 0x42, 0xe0, 0x2a,
                                  0, 0, 0, 1, 0x68, 0xce, 0x06, 0xe2,
                                  0, 0, 0, 1, 0x65};
    for (std::size_t i = 0; i < 40 * 1024; ++i)
        data.push_back(static_cast<std::uint8_t>(0x20 + ((i + sender * 7) % 95)));
    return data;
}

unsigned indexFromName(const std::string& name) {
    if (name.size() != 6 || name.rfind("peer-", 0) || name[5] < '0' || name[5] > '3')
        throw std::runtime_error("Unexpected authenticated peer name: " + name);
    return static_cast<unsigned>(name[5] - '0');
}

struct Client {
    mutable std::mutex mutex;
    std::string name;
    std::map<std::string, std::string> peers;
    std::set<std::string> connectedNames;
    std::map<std::pair<std::string, AudioKind>, std::size_t> audioPackets;
    std::map<std::string, std::size_t> videoFrames;
    std::map<std::string, std::pair<std::string, bool>> shares;
    std::map<std::pair<std::string, AudioKind>, std::uint64_t> lastAudioPts;
    std::map<std::string, std::uint64_t> lastVideoPts;
    std::size_t keyframeRequests{};
    std::vector<std::string> errors;
    std::string state, terminal;
    std::unique_ptr<RtcTransport> transport;

    explicit Client(unsigned index) : name("peer-" + std::to_string(index)) {
        RtcTransport::Callbacks callbacks;
        callbacks.peerJoined = [this](std::string id, std::string remoteName) {
            std::lock_guard lock(mutex);
            peers[std::move(id)] = std::move(remoteName);
        };
        callbacks.peerLeft = [this](std::string id) {
            std::lock_guard lock(mutex);
            if (auto found = peers.find(id); found != peers.end()) connectedNames.erase(found->second);
            peers.erase(id);
            shares.erase(id);
        };
        callbacks.audio = [this](std::string id, AudioKind kind,
                                 std::vector<std::uint8_t> bytes, std::uint64_t pts) {
            std::lock_guard lock(mutex);
            try {
                const auto& remoteName = peers.at(id);
                if (remoteName == name || bytes != audioFixture(indexFromName(remoteName), kind))
                    throw std::runtime_error("Audio source/payload mismatch");
                const auto key = std::make_pair(id, kind);
                if (lastAudioPts.contains(key) && pts < lastAudioPts[key])
                    throw std::runtime_error("Audio timestamp moved backwards");
                lastAudioPts[key] = pts;
                ++audioPackets[key];
            } catch (const std::exception& error) { errors.push_back(error.what()); }
        };
        callbacks.video = [this](std::string id, EncodedVideo frame) {
            std::lock_guard lock(mutex);
            try {
                const auto& remoteName = peers.at(id);
                if (remoteName == name || !frame.keyframe || frame.bytes != videoFixture(indexFromName(remoteName)))
                    throw std::runtime_error("Fragmented H264 source/payload mismatch");
                if (lastVideoPts.contains(id) && frame.ptsUs < lastVideoPts[id])
                    throw std::runtime_error("Video timestamp moved backwards");
                lastVideoPts[id] = frame.ptsUs;
                ++videoFrames[id];
            } catch (const std::exception& error) { errors.push_back(error.what()); }
        };
        callbacks.state = [this](std::string message) {
            std::lock_guard lock(mutex);
            if (message.rfind("Conectado com ", 0) == 0) connectedNames.insert(message.substr(14));
            state = std::move(message);
        };
        callbacks.shareState = [this](std::string id, std::string source, bool active) {
            std::lock_guard lock(mutex);
            shares[std::move(id)] = {std::move(source), active};
        };
        callbacks.keyframeRequested = [this] {
            std::lock_guard lock(mutex);
            ++keyframeRequests;
        };
        callbacks.stopped = [this](std::string reason) {
            std::lock_guard lock(mutex);
            terminal = std::move(reason);
        };
        transport = std::make_unique<RtcTransport>(std::move(callbacks));
    }

    ~Client() { transport->leave(); }

    void join(const std::string& url, const std::string& token) {
        nativecall::ConnectionSettings config;
        config.signalingUrl = url;
        config.accessToken = token;
        config.room = "transport-integration";
        config.displayName = name;
        transport->join(std::move(config));
    }

    bool connected(std::size_t count) const {
        std::lock_guard lock(mutex);
        return peers.size() == count && connectedNames.size() == count;
    }

    bool audioFromEveryPeer(std::size_t minimum = 1) const {
        std::lock_guard lock(mutex);
        for (const auto& [id, remoteName] : peers) {
            (void)remoteName;
            for (const auto kind : {AudioKind::Microphone, AudioKind::Shared}) {
                auto found = audioPackets.find({id, kind});
                if (found == audioPackets.end() || found->second < minimum) return false;
            }
        }
        return true;
    }

    bool videoFromEveryPeer() const {
        std::lock_guard lock(mutex);
        return std::all_of(peers.begin(), peers.end(), [&](const auto& peer) {
            const auto found = videoFrames.find(peer.first);
            return found != videoFrames.end() && found->second > 0;
        });
    }

    std::string idFor(const std::string& remoteName) const {
        std::lock_guard lock(mutex);
        for (const auto& [id, peerName] : peers) if (peerName == remoteName) return id;
        return {};
    }

    bool hasShare(const std::string& id, const std::string& source, bool active) const {
        std::lock_guard lock(mutex);
        const auto found = shares.find(id);
        return found != shares.end() && found->second == std::make_pair(source, active);
    }

    void checkErrors() const {
        std::lock_guard lock(mutex);
        if (!errors.empty()) throw std::runtime_error(name + ": " + errors.front());
        if (!terminal.empty()) throw std::runtime_error(name + ": " + terminal);
    }

    void dump() const {
        std::lock_guard lock(mutex);
        std::cerr << name << ": peers=" << peers.size() << " connected=" << connectedNames.size()
                  << " audioSources=" << audioPackets.size() << " videoSources=" << videoFrames.size()
                  << " state=" << state << " terminal=" << terminal << '\n';
    }
};

template<class Predicate>
void waitUntil(Predicate predicate, std::chrono::seconds timeout, const char* failure) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(failure);
        std::this_thread::sleep_for(10ms);
    }
}

template<class Range>
void sendAudio(const Range& clients) {
    const auto now = nativecall::mediaNowUs();
    for (const auto& client : clients) {
        const auto index = indexFromName(client->name);
        for (const auto kind : {AudioKind::Microphone, AudioKind::Shared})
            client->transport->sendAudio(kind, audioFixture(index, kind), now);
    }
}

} // namespace

int main() {
    std::array<std::unique_ptr<Client>, 4> clients;
    try {
        const auto urlValue = std::getenv("NATIVE_CALL_SIGNALING_URL");
        const auto tokenValue = std::getenv("NATIVE_CALL_TOKEN");
        if (!urlValue || !tokenValue)
            throw std::runtime_error("Use tests/transport_runner.py to start the signaling server.");
        const std::string url = urlValue, token = tokenValue;
        rtc::InitLogger(rtc::LogLevel::Warning);
        for (unsigned i = 0; i < clients.size(); ++i) {
            clients[i] = std::make_unique<Client>(i);
            clients[i]->join(url, token);
        }
        waitUntil([&] {
            for (const auto& client : clients) client->checkErrors();
            return std::all_of(clients.begin(), clients.end(), [](const auto& c) { return c->connected(3); });
        }, 20s, "Four-client WebRTC mesh did not establish all twelve peer connections.");

        for (int iteration = 0; iteration < 60; ++iteration) {
            sendAudio(clients);
            std::this_thread::sleep_for(10ms);
        }
        waitUntil([&] {
            for (const auto& client : clients) client->checkErrors();
            return std::all_of(clients.begin(), clients.end(), [](const auto& c) { return c->audioFromEveryPeer(10); });
        }, 5s, "Distinct microphone/shared-audio RTP payloads did not reach every peer.");

        for (int iteration = 0; iteration < 12; ++iteration) {
            const auto now = nativecall::mediaNowUs();
            for (unsigned i = 0; i < clients.size(); ++i)
                clients[i]->transport->sendVideo({videoFixture(i), now, true});
            sendAudio(clients); // Voice continues while all four peers send screen video.
            std::this_thread::sleep_for(50ms);
        }
        waitUntil([&] {
            for (const auto& client : clients) client->checkErrors();
            return std::all_of(clients.begin(), clients.end(), [](const auto& c) { return c->videoFromEveryPeer(); });
        }, 5s, "Fragmented H264 RTP access units were not reassembled from every sender.");
        for (const auto& client : clients) {
            std::lock_guard lock(client->mutex);
            for (const auto& [id, remoteName] : client->peers) {
                (void)remoteName;
                const auto microphone = client->lastAudioPts.at({id, AudioKind::Microphone});
                const auto shared = client->lastAudioPts.at({id, AudioKind::Shared});
                const auto video = client->lastVideoPts.at(id);
                const auto gap = shared > video ? shared - video : video - shared;
                if (microphone != shared || gap > 100000)
                    throw std::runtime_error("Audio/video RTP clocks do not share the sender epoch.");
            }
        }

        const auto senderId = clients[1]->idFor(clients[0]->name);
        clients[0]->transport->announceShare("Monitor 1", true);
        waitUntil([&] {
            return clients[1]->hasShare(senderId, "Monitor 1", true) &&
                   clients[2]->hasShare(senderId, "Monitor 1", true) &&
                   clients[3]->hasShare(senderId, "Monitor 1", true);
        }, 5s, "Share metadata not delivered.");
        clients[0]->transport->announceShare("Jogo selecionado", true);
        waitUntil([&] {
            return clients[1]->hasShare(senderId, "Jogo selecionado", true) &&
                   clients[2]->hasShare(senderId, "Jogo selecionado", true) &&
                   clients[3]->hasShare(senderId, "Jogo selecionado", true);
        }, 5s, "Share source change did not preserve the call.");

        std::size_t keyframesBefore;
        { std::lock_guard lock(clients[0]->mutex); keyframesBefore = clients[0]->keyframeRequests; }
        std::this_thread::sleep_for(350ms); // Sender deliberately rate-limits PLI callbacks.
        clients[1]->transport->requestRemoteKeyframe(senderId);
        waitUntil([&] {
            std::lock_guard lock(clients[0]->mutex);
            return clients[0]->keyframeRequests > keyframesBefore;
        }, 5s, "RTCP PLI did not reach the requested sender.");

        clients[0]->transport->announceShare({}, false);
        waitUntil([&] {
            return clients[1]->hasShare(senderId, {}, false) &&
                   clients[2]->hasShare(senderId, {}, false) &&
                   clients[3]->hasShare(senderId, {}, false);
        }, 5s, "Stopping the share was not announced independently from call audio.");

        clients[3]->transport->leave();
        waitUntil([&] {
            return clients[0]->connected(2) && clients[1]->connected(2) && clients[2]->connected(2);
        }, 5s, "A participant leaving interrupted the remaining mesh.");
        std::array<std::size_t, 3> previous{};
        for (std::size_t i = 0; i < previous.size(); ++i) {
            std::lock_guard lock(clients[i]->mutex);
            for (const auto& [key, count] : clients[i]->audioPackets) { (void)key; previous[i] += count; }
        }
        for (int iteration = 0; iteration < 25; ++iteration) {
            sendAudio(clients);
            std::this_thread::sleep_for(10ms);
        }
        for (std::size_t i = 0; i < previous.size(); ++i) {
            clients[i]->checkErrors();
            std::lock_guard lock(clients[i]->mutex);
            std::size_t total{};
            for (const auto& [key, count] : clients[i]->audioPackets) { (void)key; total += count; }
            if (total <= previous[i]) throw std::runtime_error("Audio stopped after a participant left.");
        }
        std::cout << "PASS: four real WebRTC clients, twelve peer connections, independent voice/shared RTP,\n"
                     "      fragmented H264/SRTP round-trip, PLI, share switch/stop, and leave continuity.\n";
        for (auto& client : clients) client->transport->leave();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        for (const auto& client : clients) if (client) client->dump();
        return 1;
    }
}
