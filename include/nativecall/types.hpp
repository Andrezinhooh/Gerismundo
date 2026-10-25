#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nativecall {

enum class AudioKind { Microphone, Shared };
enum class SourceKind { Monitor, Window };

struct CaptureSource {
    SourceKind kind{SourceKind::Monitor};
    std::uintptr_t handle{};
    std::uint32_t processId{};
    std::wstring name;
};

struct AudioDevice { std::wstring id, name; };

struct VideoQuality {
    std::uint32_t width{1280}, height{720}, fps{30}, bitrate{2500000};
};

struct VideoFrame {
    std::uint32_t width{}, height{}, stride{};
    std::vector<std::uint8_t> bgra;
    std::uint64_t ptsUs{};
};

struct EncodedVideo {
    std::vector<std::uint8_t> bytes;
    std::uint64_t ptsUs{};
    bool keyframe{};
};

struct IceServer { std::string url, username, password; };

struct ConnectionSettings {
    std::string signalingUrl{"ws://127.0.0.1:8080"};
    std::string room{"amigos"};
    std::string displayName{"Participante"};
    std::string accessToken;
    std::vector<IceServer> iceServers;
};

struct ParticipantSnapshot {
    std::string id, name;
    bool speaking{}, sharing{};
    float voiceVolume{1.0f}, shareVolume{1.0f};
    std::string sharedSource;
    std::shared_ptr<const VideoFrame> video;
};

struct AppSnapshot {
    bool joined{}, joining{}, micMuted{}, deafened{}, sharing{}, shareMuted{};
    std::wstring sourceName;
    std::string connectionState{"Desconectado"};
    std::vector<ParticipantSnapshot> participants;
    VideoQuality quality;
};

inline std::uint64_t mediaNowUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace nativecall
