#pragma once

#include "nativecall/types.hpp"
#include <opus/opus.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// Internal, platform-independent Opus and bounded playout implementation.
// Keeping this shared with tests validates the production code without WASAPI.
namespace nativecall::audio_detail {

inline constexpr int kRate = 48000;
inline constexpr int kFrames = 480;
inline constexpr int kChannels = 2;
inline constexpr std::uint64_t kFrameUs = 10000;
using Stereo = std::array<opus_int16, kFrames * kChannels>;

class Encoder final {
    OpusEncoder* value_{};
public:
    explicit Encoder(AudioKind kind) {
        int error{};
        value_ = opus_encoder_create(kRate, kChannels,
            kind == AudioKind::Microphone ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO, &error);
        if (!value_ || error != OPUS_OK) throw std::runtime_error("Falha criando codificador Opus");
        opus_encoder_ctl(value_, OPUS_SET_BITRATE(kind == AudioKind::Microphone ? 64000 : 128000));
        opus_encoder_ctl(value_, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(value_, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(value_, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(value_, OPUS_SET_DTX(kind == AudioKind::Microphone ? 1 : 0));
    }
    ~Encoder() { if (value_) opus_encoder_destroy(value_); }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    std::vector<std::uint8_t> encode(const Stereo& samples) {
        std::vector<std::uint8_t> packet(4000);
        const int size = opus_encode(value_, samples.data(), kFrames, packet.data(),
                                     static_cast<opus_int32>(packet.size()));
        if (size < 0) throw std::runtime_error(opus_strerror(size));
        packet.resize(static_cast<std::size_t>(size));
        return packet;
    }
};

struct Remote final {
    OpusDecoder* decoder{};
    std::map<std::uint64_t, std::vector<std::uint8_t>> packets;
    std::uint64_t nextPts{};
    std::uint64_t firstArrival{};
    int missed{};
    float volume{1.0f};
    bool buffered{}, speaking{};
    std::uint64_t voiceUntil{};
    std::optional<std::uint64_t> lastPlayed;

    Remote() {
        int error{};
        decoder = opus_decoder_create(kRate, kChannels, &error);
        if (!decoder || error != OPUS_OK) throw std::runtime_error("Falha criando decodificador Opus");
    }
    ~Remote() { if (decoder) opus_decoder_destroy(decoder); }
    Remote(const Remote&) = delete;
    Remote& operator=(const Remote&) = delete;

    void push(std::span<const std::uint8_t> packet, std::uint64_t pts) {
        if (packet.empty() || packet.size() > 4000) return;
        // Validate duration before handing untrusted media to the render loop.
        if (opus_packet_get_nb_samples(packet.data(), static_cast<opus_int32>(packet.size()), kRate)
            != kFrames) return;
        if (buffered && pts < nextPts) return;
        if (packets.empty()) firstArrival = mediaNowUs();
        packets.try_emplace(pts, packet.begin(), packet.end());
        if (packets.size() > 24) {
            while (packets.size() > 6) packets.erase(packets.begin());
            nextPts = packets.begin()->first;
            buffered = false;
            opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        }
    }

    bool decode(Stereo& samples) {
        samples.fill(0);
        if (!buffered) {
            if (packets.empty() || (packets.size() < 3 && mediaNowUs() - firstArrival < 30000)) return false;
            nextPts = packets.begin()->first;
            buffered = true;
            missed = 0;
        }
        auto packet = packets.find(nextPts);
        const std::uint8_t* bytes{};
        opus_int32 length{};
        bool fec{};
        if (packet != packets.end()) {
            bytes = packet->second.data();
            length = static_cast<opus_int32>(packet->second.size());
            missed = 0;
        } else if (++missed > 6) {
            buffered = false;
            // Packets may have a timestamp discontinuity after a source restart.
            // Rebuffer from its first available timestamp rather than growing lag.
            return false;
        } else {
            const auto following = packets.find(nextPts + kFrameUs);
            if (following != packets.end()) {
                bytes = following->second.data();
                length = static_cast<opus_int32>(following->second.size());
                fec = true;
            }
        }
        const int decoded = opus_decode(decoder, bytes, length, samples.data(), kFrames, fec ? 1 : 0);
        if (packet != packets.end()) packets.erase(packet);
        lastPlayed = nextPts;
        nextPts += kFrameUs;
        while (!packets.empty() && packets.begin()->first < nextPts) packets.erase(packets.begin());
        if (decoded != kFrames) { samples.fill(0); return false; }
        return true;
    }
};

} // namespace nativecall::audio_detail
