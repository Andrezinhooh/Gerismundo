#pragma once

#include "nativecall/types.hpp"
#include "nativecall/capture.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace nativecall {

// One owned GPU frame is pending at a time. submit() never waits for the codec.
// Output is H.264 Annex B with SPS/PPS repeated at every IDR.
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    void start(VideoQuality quality, std::function<void(EncodedVideo)> onFrame,
               std::function<void(std::string)> onError = {});
    void submit(GpuFrame frame);
    void requestKeyframe();
    void setBitrate(std::uint32_t bitrate);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Calls are serialized; decoded BGRA data owns its storage. reset() drops any
// delayed pictures when a sender stops, reconnects, or changes resolution.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    std::optional<VideoFrame> decode(std::span<const std::uint8_t> annexB,
                                     std::uint64_t ptsUs);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nativecall
