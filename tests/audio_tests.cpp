#include "../src/audio_codec.hpp"

#include <array>
#include <iostream>
#include <string>

using namespace nativecall;
using namespace nativecall::audio_detail;

namespace {
int checks{};
void require(bool result, const char* explanation) {
    ++checks;
    if (!result) throw std::runtime_error(explanation);
}
}

int main() {
    try {
        Encoder encoder(AudioKind::Microphone);
        Stereo samples{};
        for (int i = 0; i != kFrames; ++i) {
            samples[i * 2] = samples[i * 2 + 1] = static_cast<opus_int16>((i % 60 - 30) * 350);
        }
        const auto packet = encoder.encode(samples);
        require(opus_packet_get_nb_samples(packet.data(), static_cast<opus_int32>(packet.size()),
            kRate) == kFrames, "Production encoder must produce 10 ms Opus packets");

        Remote stream;
        stream.push(packet, 20000);
        stream.push(packet, 0);
        stream.push(packet, 10000);
        stream.push(packet, 10000);
        require(stream.packets.size() == 3, "Duplicate packets must be ignored");
        Stereo decoded{};
        require(stream.decode(decoded) && stream.lastPlayed == 0, "Reordered packet 0 must play first");
        require(stream.decode(decoded) && stream.lastPlayed == 10000, "Packet 10 ms must follow");
        require(stream.decode(decoded) && stream.lastPlayed == 20000, "Packet 20 ms must follow");
        stream.push(packet, 0);
        require(stream.packets.empty(), "Late packets must be dropped");
        require(stream.decode(decoded) && stream.lastPlayed == 30000,
                "One missing packet must produce bounded Opus packet-loss concealment");

        stream.push(packet, 50000);
        require(stream.decode(decoded) && stream.lastPlayed == 40000,
                "FEC decode path must advance the missing packet timestamp");
        require(stream.packets.contains(50000), "A packet used for FEC must be retained for normal playback");
        require(stream.decode(decoded) && stream.lastPlayed == 50000,
                "The retained next packet must play at its own timestamp");

        Remote bounded;
        bool remainedBounded = true;
        for (std::uint64_t index = 0; index != 1000; ++index) {
            bounded.push(packet, index * kFrameUs);
            remainedBounded = remainedBounded && bounded.packets.size() <= 24;
        }
        require(remainedBounded, "Incoming audio queue must remain bounded when render is stalled");

        Remote invalid;
        const std::array<std::uint8_t, 1> malformed{0xff};
        invalid.push(malformed, 0);
        require(invalid.packets.empty(), "Malformed Opus must not enter the playout queue");

        int error{};
        auto* encoder20 = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_AUDIO, &error);
        require(error == OPUS_OK && encoder20, "Test 20 ms Opus encoder must initialize");
        std::array<opus_int16, 960 * 2> pcm20{};
        std::array<std::uint8_t, 4000> packet20{};
        const auto length20 = opus_encode(encoder20, pcm20.data(), 960, packet20.data(),
                                         static_cast<opus_int32>(packet20.size()));
        opus_encoder_destroy(encoder20);
        require(length20 > 0, "Test 20 ms Opus packet must encode");
        invalid.push(std::span(packet20.data(), static_cast<std::size_t>(length20)), 0);
        require(invalid.packets.empty(), "Unexpected 20 ms media cannot enter the fixed 10 ms playout queue");

        stream.push(packet, 1000000);
        stream.push(packet, 1010000);
        stream.push(packet, 1020000);
        for (int index = 0; index != 8; ++index) stream.decode(decoded);
        require(stream.lastPlayed && *stream.lastPlayed >= 1000000,
                "Timestamp discontinuity must recover without unbounded latency");

        Encoder music(AudioKind::Shared);
        Remote shared;
        const auto musicPacket = music.encode(samples);
        shared.push(musicPacket, 0);
        shared.push(musicPacket, 10000);
        shared.push(musicPacket, 20000);
        require(shared.decode(decoded) && shared.lastPlayed == 0,
                "Shared-audio codec and playout must have state independent of microphone");
        require(stream.lastPlayed && *stream.lastPlayed >= 1000000,
                "Playing shared audio must not advance microphone state");

        std::cout << "PASS: " << checks << " production Opus/jitter checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
