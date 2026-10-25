#pragma once

#include "nativecall/types.hpp"
#include <functional>
#include <memory>
#include <span>

namespace nativecall {

// All user callbacks run on the transport's serial worker. They must return
// promptly. join()/leave() are called from the controller, never a callback.
class RtcTransport {
public:
    using Config = ConnectionSettings;
    struct Callbacks {
        std::function<void(std::string, std::string)> peerJoined;
        std::function<void(std::string)> peerLeft;
        std::function<void(std::string, AudioKind, std::vector<std::uint8_t>, std::uint64_t)> audio;
        std::function<void(std::string, EncodedVideo)> video;
        std::function<void(std::string)> state;
        std::function<void(std::string, std::string, bool)> shareState;
        std::function<void()> keyframeRequested;
        std::function<void(std::uint32_t)> bitrateSuggested;
        std::function<void(std::string)> stopped;
    };

    explicit RtcTransport(Callbacks callbacks = {});
    ~RtcTransport();
    RtcTransport(const RtcTransport&) = delete;
    RtcTransport& operator=(const RtcTransport&) = delete;

    void join(Config config);
    void leave();
    void sendAudio(AudioKind kind, std::span<const std::uint8_t> opus, std::uint64_t ptsUs);
    void sendVideo(const EncodedVideo& frame);
    void announceShare(std::string sourceName, bool active);
    void requestRemoteKeyframe(std::string peerId);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace nativecall
