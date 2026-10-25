// Native Media Foundation integration test. This fixture is confined to tests:
// the application itself obtains textures only from Windows Graphics Capture.
#include "nativecall/video.hpp"

#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using nativecall::EncodedVideo;
using nativecall::GpuFrame;
using nativecall::VideoDecoder;
using nativecall::VideoEncoder;
using nativecall::VideoFrame;
using nativecall::VideoQuality;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream message;
    message << operation << " failed: HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result);
    throw std::runtime_error(message.str());
}

struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    Apartment() { check(result, "CoInitializeEx"); }
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

ComPtr<ID3D11Texture2D> fixtureTexture(const VideoQuality& quality) {
    // WARP exercises real Direct3D and MF on the Windows runner without assuming
    // it has a physical GPU, an interactive source window, or a microphone.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "D3D11CreateDevice(WARP)");
    ComPtr<ID3D10Multithread> multithread;
    check(context.As(&multithread), "ID3D10Multithread");
    multithread->SetMultithreadProtected(TRUE);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(quality.width) * quality.height * 4);
    for (std::uint32_t y = 0; y != quality.height; ++y) {
        for (std::uint32_t x = 0; x != quality.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * quality.width + x) * 4;
            const bool left = x < quality.width / 2;
            pixels[offset] = left ? 30 : 190;       // B
            pixels[offset + 1] = left ? 50 : 160;   // G
            pixels[offset + 2] = left ? 200 : 20;   // R
            pixels[offset + 3] = 255;
        }
    }
    D3D11_TEXTURE2D_DESC descriptor{};
    descriptor.Width = quality.width;
    descriptor.Height = quality.height;
    descriptor.MipLevels = descriptor.ArraySize = 1;
    descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descriptor.SampleDesc.Count = 1;
    descriptor.Usage = D3D11_USAGE_DEFAULT;
    descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = quality.width * 4;
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&descriptor, &initial, &texture), "Create fixture texture");
    return texture;
}

struct NalKinds { bool sps{}, pps{}, idr{}; };
NalKinds nalKinds(const std::vector<std::uint8_t>& bytes) {
    NalKinds found;
    for (std::size_t offset = 0; offset + 3 < bytes.size(); ++offset) {
        if (bytes[offset] || bytes[offset + 1]) continue;
        std::size_t prefix{};
        if (bytes[offset + 2] == 1) prefix = 3;
        else if (offset + 4 < bytes.size() && !bytes[offset + 2] && bytes[offset + 3] == 1) prefix = 4;
        if (!prefix || offset + prefix >= bytes.size()) continue;
        const auto type = bytes[offset + prefix] & 31;
        found.sps = found.sps || type == 7;
        found.pps = found.pps || type == 8;
        found.idr = found.idr || type == 5;
        offset += prefix;
    }
    return found;
}

void validatePicture(const VideoFrame& frame, const VideoQuality& quality,
                     std::uint64_t firstPts, std::uint64_t& lastPts) {
    require(frame.width == quality.width && frame.height == quality.height,
            "Decoded resolution differs from requested source (including display aperture)");
    require(frame.stride >= frame.width * 4 &&
            frame.bgra.size() >= static_cast<std::size_t>(frame.stride) * frame.height,
            "Decoded BGRA allocation is truncated");
    require(frame.ptsUs >= firstPts && frame.ptsUs >= lastPts,
            "Decoder failed to preserve monotonic source timestamps: actual=" +
            std::to_string(frame.ptsUs) + " first=" + std::to_string(firstPts) +
            " previous=" + std::to_string(lastPts));
    lastPts = frame.ptsUs;
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return frame.bgra.data() + static_cast<std::size_t>(y) * frame.stride + x * 4;
    };
    const auto* left = pixel(frame.width / 4, frame.height / 2);
    const auto* right = pixel(frame.width * 3 / 4, frame.height / 2);
    // Relative channel checks tolerate chroma subsampling, quantization, and
    // driver colorimetry differences, while rejecting black/empty or swapped RGB.
    require(left[2] > left[0] + 70 && left[2] > left[1] + 50,
            "Decoded red region did not survive BGRA/NV12/H264 conversion");
    require(right[0] > right[2] + 70 && right[1] > right[2] + 60,
            "Decoded cyan region did not survive BGRA/NV12/H264 conversion");
    require(left[3] == 255 && right[3] == 255, "Decoded BGRA alpha is not opaque");
}

void roundTrip(VideoDecoder& decoder, VideoQuality quality, std::uint64_t firstPts) {
    auto texture = fixtureTexture(quality);
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<EncodedVideo> packets;
    std::string error;
    VideoEncoder encoder;
    encoder.start(quality,
        [&](EncodedVideo packet) {
            std::lock_guard lock(mutex);
            if (packets.size() == 16) {
                error = "Test did not consume the native encoder output promptly";
            } else packets.push_back(std::move(packet));
            wake.notify_one();
        },
        [&](std::string message) {
            std::lock_guard lock(mutex);
            error = std::move(message);
            wake.notify_one();
        });
    encoder.requestKeyframe();

    auto nextSubmit = std::chrono::steady_clock::now();
    const auto deadline = nextSubmit + 12s;
    std::uint64_t lastPacketPts{}, lastPicturePts{};
    std::uint64_t submitted{};
    unsigned decoded{};
    bool independentFrame{};
    while (decoded < 2 && std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSubmit) {
            encoder.submit(GpuFrame{texture, firstPts + submitted * 33333,
                                    quality.width, quality.height});
            ++submitted;
            nextSubmit = now + 34ms;
        }
        std::optional<EncodedVideo> packet;
        {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, 10ms, [&] { return !packets.empty() || !error.empty(); });
            if (!error.empty()) throw std::runtime_error("Native encoder: " + error);
            if (!packets.empty()) {
                packet = std::move(packets.front());
                packets.pop_front();
            }
        }
        if (!packet) continue;
        require(!packet->bytes.empty(), "Native encoder emitted an empty picture");
        require(packet->ptsUs >= firstPts && packet->ptsUs >= lastPacketPts,
                "Encoder output timestamps moved backwards");
        lastPacketPts = packet->ptsUs;
        const auto nals = nalKinds(packet->bytes);
        if (packet->keyframe) {
            require(nals.idr && nals.sps && nals.pps,
                    "IDR access unit is not independently decodable (SPS/PPS missing)");
            independentFrame = true;
        }
        if (auto picture = decoder.decode(packet->bytes, packet->ptsUs)) {
            validatePicture(*picture, quality, firstPts, lastPicturePts);
            ++decoded;
        }
    }
    encoder.stop();
    require(independentFrame, "Encoder did not emit an independently decodable IDR");
    require(decoded >= 2, "Timed out waiting for two decoded native H264 pictures");
    {
        std::lock_guard lock(mutex);
        require(error.empty(), "Encoder failed during stop: " + error);
    }
    std::cout << "PASS native MF: " << quality.width << 'x' << quality.height
              << " BGRA -> NV12 -> H264 -> BGRA, IDR SPS/PPS, pixels, timestamps, stop\n";
}
} // namespace

int main() try {
    Apartment apartment;
    VideoDecoder decoder;
    roundTrip(decoder, VideoQuality{1280, 720, 30, 2500000}, 2000000);
    decoder.reset();
    roundTrip(decoder, VideoQuality{1920, 1080, 30, 4000000}, 20000000);
    decoder.reset();
    std::cout << "PASS: native Media Foundation encode/decode at 720p and 1080p; "
                 "reset/source-size change completed. No physical capture or audio was tested.\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL native Windows media: " << error.what() << '\n';
    return 1;
}
