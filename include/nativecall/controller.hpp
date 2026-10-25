#pragma once

#include "types.hpp"
#include <memory>

namespace nativecall {

// Serializes potentially blocking native operations away from the window thread.
// Callbacks may originate on worker threads; the UI must marshal them with PostMessage.
class CallController final {
public:
    struct Callbacks {
        std::function<void()> changed;
        std::function<void(std::string)> error;
    };
    explicit CallController(Callbacks callbacks = {});
    ~CallController();
    CallController(const CallController&) = delete;
    CallController& operator=(const CallController&) = delete;

    void join(ConnectionSettings settings);
    void leave();
    void setMicrophoneMuted(bool muted);
    void setDeafened(bool deafened);
    void selectDevices(std::wstring input, std::wstring output);
    void setVolume(std::string peer, AudioKind kind, float gain);
    void startSharing(CaptureSource source, VideoQuality quality, bool withAudio);
    void stopSharing();
    void setShareMuted(bool muted);
    AppSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nativecall
