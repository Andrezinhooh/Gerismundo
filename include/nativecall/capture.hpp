#pragma once

#include "nativecall/types.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nativecall {

// A retained copy on the capture D3D11 device. The consumer may retain it after
// the callback returns; a WGC frame-pool surface is never exposed to consumers.
struct GpuFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::uint64_t ptsUs{};
    std::uint32_t width{};
    std::uint32_t height{};
};

class ScreenCapture final {
public:
    using FrameCallback = std::function<void(GpuFrame)>;
    using ErrorCallback = std::function<void(std::string)>;

    ScreenCapture();
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Startup, capture and frame delivery take place off the caller's thread.
    // The most recent frame replaces an undelivered frame when encoding lags.
    // Errors (including a closed source) are delivered on the delivery thread.
    void start(const CaptureSource& source, const VideoQuality& quality,
               FrameCallback onFrame, ErrorCallback onError);
    void stop();

    // Re-enumerate every time the selector is opened. Does not retain HWNDs or
    // process handles; start() revalidates the selected HWND/PID pair.
    static std::vector<CaptureSource> enumerateSources(SourceKind kind);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nativecall
