#pragma once

#include "types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nativecall {

enum class AudioFailure { Microphone, Output, Shared };

// All packet callbacks use stereo Opus, 48 kHz, 480 samples/channel (10 ms).
// Callbacks run on audio workers and must not synchronously call stop().
class AudioEngine final {
public:
    struct Callbacks {
        std::function<void(AudioKind, std::vector<std::uint8_t>, std::uint64_t)> onPacket;
        std::function<void(std::string, bool)> onActivity;
        std::function<void(AudioFailure, std::string)> onError;
    };

    explicit AudioEngine(Callbacks callbacks = {});
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Empty IDs select the Windows default communications endpoints. To change
    // endpoints, stop and start again; the chosen IDs come from enumerateDevices.
    void start(std::wstring inputDeviceId = {}, std::wstring outputDeviceId = {});
    void stop() noexcept;
    void setMicrophoneMuted(bool muted) noexcept;
    void setDeafened(bool deafened) noexcept;
    void startShareAudio(const CaptureSource& source);
    void stopShareAudio() noexcept;
    void setShareMuted(bool muted) noexcept;
    void setVolume(std::string peerId, AudioKind kind, float volume);
    void receive(std::string peerId, AudioKind kind,
                 std::span<const std::uint8_t> opusPacket, std::uint64_t ptsUs);
    void removePeer(std::string peerId);
    std::optional<std::uint64_t> playoutPosition(const std::string& peerId, AudioKind kind) const;

    static std::vector<AudioDevice> enumerateDevices(bool input);
    static bool selectiveLoopbackSupported() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nativecall
