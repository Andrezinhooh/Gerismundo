#include "nativecall/video.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace nativecall {
namespace {
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

[[noreturn]] void fail(HRESULT hr, const char* operation) {
    char detail[32]{};
    std::snprintf(detail, sizeof(detail), " (HRESULT 0x%08lX)", static_cast<unsigned long>(hr));
    throw std::runtime_error(std::string(operation) + detail);
}
void check(HRESULT hr, const char* operation) { if (FAILED(hr)) fail(hr, operation); }
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComScope() { if (FAILED(result) && result != RPC_E_CHANGED_MODE) fail(result, "CoInitializeEx"); }
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct MfScope {
    MfScope() { check(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup"); }
    ~MfScope() { MFShutdown(); }
};

ComPtr<IMFMediaType> videoType(GUID subtype, const VideoQuality& q) {
    ComPtr<IMFMediaType> type;
    check(MFCreateMediaType(&type), "MFCreateMediaType");
    check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Media type major");
    check(type->SetGUID(MF_MT_SUBTYPE, subtype), "Media type subtype");
    check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, q.width, q.height), "Frame size");
    check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, q.fps, 1), "Frame rate");
    check(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Pixel aspect");
    check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Interlace mode");
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}
void setCodec(IMFTransform* transform, const GUID& property, ULONG value, bool boolean = false) {
    ComPtr<ICodecAPI> codec;
    if (SUCCEEDED(transform->QueryInterface(IID_PPV_ARGS(&codec)))) {
        VARIANT setting;
        VariantInit(&setting);
        setting.vt = boolean ? VT_BOOL : VT_UI4;
        if (boolean) setting.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else setting.ulVal = value;
        codec->SetValue(&property, &setting);
    }
}

bool hasStartCode(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 3 && bytes[0] == 0 && bytes[1] == 0 &&
           (bytes[2] == 1 || (bytes.size() >= 4 && bytes[2] == 0 && bytes[3] == 1));
}
std::vector<std::uint8_t> toAnnexB(std::span<const std::uint8_t> input) {
    if (hasStartCode(input)) return {input.begin(), input.end()};
    std::vector<std::uint8_t> result;
    std::size_t offset = 0;
    while (offset + 4 <= input.size()) {
        const auto length = (std::uint32_t(input[offset]) << 24) |
                            (std::uint32_t(input[offset + 1]) << 16) |
                            (std::uint32_t(input[offset + 2]) << 8) | input[offset + 3];
        offset += 4;
        if (!length || length > input.size() - offset) return {};
        result.insert(result.end(), {0, 0, 0, 1});
        result.insert(result.end(), input.begin() + offset, input.begin() + offset + length);
        offset += length;
    }
    if (offset != input.size()) return {};
    return result;
}
std::vector<std::uint8_t> sequenceAnnexB(std::span<const std::uint8_t> data) {
    if (hasStartCode(data)) return {data.begin(), data.end()};
    // Some hardware encoders expose an AVCDecoderConfigurationRecord.
    if (data.size() < 7 || data[0] != 1) return toAnnexB(data);
    std::vector<std::uint8_t> result;
    std::size_t offset = 6;
    auto append = [&](unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            if (offset + 2 > data.size()) return false;
            const auto length = (unsigned(data[offset]) << 8) | data[offset + 1];
            offset += 2;
            if (length > data.size() - offset) return false;
            result.insert(result.end(), {0, 0, 0, 1});
            result.insert(result.end(), data.begin() + offset, data.begin() + offset + length);
            offset += length;
        }
        return true;
    };
    if (!append(data[5] & 31) || offset >= data.size()) return {};
    const unsigned pps = data[offset++];
    if (!append(pps)) return {};
    return result;
}
// Inspect Annex B NAL units without interpreting slice contents.
std::vector<std::span<const std::uint8_t>> nals(std::span<const std::uint8_t> data) {
    std::vector<std::span<const std::uint8_t>> result;
    std::size_t start = 0;
    auto find = [&](std::size_t pos) {
        for (; pos + 3 <= data.size(); ++pos) {
            if (data[pos] == 0 && data[pos + 1] == 0 &&
                (data[pos + 2] == 1 || (pos + 4 <= data.size() && data[pos + 2] == 0 && data[pos + 3] == 1))) return pos;
        }
        return data.size();
    };
    start = find(0);
    while (start < data.size()) {
        const auto prefix = data[start + 2] == 1 ? 3u : 4u;
        const auto next = find(start + prefix);
        if (next > start + prefix) result.emplace_back(data.data() + start + prefix, next - start - prefix);
        start = next;
    }
    return result;
}

struct D3dLock {
    ComPtr<ID3D10Multithread> multithread;
    explicit D3dLock(ID3D11Device* device) {
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if (SUCCEEDED(context.As(&multithread))) multithread->Enter();
    }
    ~D3dLock() { if (multithread) multithread->Leave(); }
};

// Shared with the capture device. Windows' video processor performs aspect-
// preserving scaling and BGRA -> studio-range BT.709 NV12 on the GPU.
class Nv12Converter {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> staging;
    UINT sourceWidth = 0, sourceHeight = 0;
    VideoQuality quality{};

    void initialize(ID3D11Texture2D* texture, VideoQuality q) {
        texture->GetDevice(&device);
        device->GetImmediateContext(&context);
        quality = q;
        D3D11_TEXTURE2D_DESC input{};
        texture->GetDesc(&input);
        sourceWidth = input.Width;
        sourceHeight = input.Height;
        videoDevice.Reset(); videoContext.Reset(); enumerator.Reset(); processor.Reset(); staging.Reset();
        if (FAILED(device.As(&videoDevice)) || FAILED(context.As(&videoContext))) return;
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = input.Width; content.InputHeight = input.Height;
        content.OutputWidth = q.width; content.OutputHeight = q.height;
        content.InputFrameRate = {q.fps, 1}; content.OutputFrameRate = {q.fps, 1};
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator)) ||
            FAILED(videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor))) {
            processor.Reset();
        }
    }
    ComPtr<ID3D11Texture2D> convert(ID3D11Texture2D* input) {
        if (!processor) return {};
        D3dLock lock(device.Get());
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = quality.width; desc.Height = quality.height;
        desc.MipLevels = desc.ArraySize = 1; desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> output;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &output))) return {};
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        if (FAILED(videoDevice->CreateVideoProcessorInputView(input, enumerator.Get(), &inputDesc, &inputView))) return {};
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> outputView;
        if (FAILED(videoDevice->CreateVideoProcessorOutputView(output.Get(), enumerator.Get(), &outputDesc, &outputView))) return {};
        RECT sourceRect{0, 0, LONG(sourceWidth), LONG(sourceHeight)};
        const double scale = std::min(double(quality.width) / sourceWidth, double(quality.height) / sourceHeight);
        const LONG w = LONG(sourceWidth * scale) & ~1L, h = LONG(sourceHeight * scale) & ~1L;
        const LONG x = (LONG(quality.width) - w) / 2, y = (LONG(quality.height) - h) / 2;
        RECT destRect{x & ~1L, y & ~1L, (x & ~1L) + w, (y & ~1L) + h};
        RECT targetRect{0, 0, LONG(quality.width), LONG(quality.height)};
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &sourceRect);
        videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &destRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &targetRect);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_VIDEO_COLOR black{}; black.RGBA.A = 1.0f;
        videoContext->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &black);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inColor{};
        inColor.RGB_Range = 0;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inColor);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outColor{};
        outColor.YCbCr_Matrix = 1; outColor.Nominal_Range = 1;
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &outColor);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = inputView.Get();
        if (FAILED(videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream))) return {};
        return output;
    }
    ComPtr<IMFMediaBuffer> cpuBuffer(ID3D11Texture2D* original, ID3D11Texture2D* nv12) {
        ID3D11Texture2D* source = nv12 ? nv12 : original;
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC existing{};
        if (staging) staging->GetDesc(&existing);
        if (!staging || existing.Width != desc.Width || existing.Height != desc.Height || existing.Format != desc.Format) {
            desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.Reset();
            check(device->CreateTexture2D(&desc, nullptr, &staging), "Create staging video texture");
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        {
            D3dLock lock(device.Get());
            context->CopyResource(staging.Get(), source);
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map captured frame");
        }
        // Release the device lock while copying/converting mapped CPU memory;
        // capture can keep submitting GPU frames during software encoding.
        struct Unmap { ID3D11DeviceContext* context; ID3D11Texture2D* texture; ~Unmap() { context->Unmap(texture, 0); } } unmap{context.Get(), staging.Get()};
        ComPtr<IMFMediaBuffer> buffer;
        const auto bytes = quality.width * quality.height * 3 / 2;
        check(MFCreateMemoryBuffer(bytes, &buffer), "Allocate NV12 buffer");
        BYTE* destination = nullptr;
        check(buffer->Lock(&destination, nullptr, nullptr), "Lock NV12 buffer");
        struct Unlock { IMFMediaBuffer* buffer; ~Unlock() { buffer->Unlock(); } } unlock{buffer.Get()};
        const BYTE* input = static_cast<const BYTE*>(mapped.pData);
        if (nv12) {
            for (UINT row = 0; row < quality.height * 3 / 2; ++row)
                std::memcpy(destination + row * quality.width, input + row * mapped.RowPitch, quality.width);
        } else {
            // A device without a video processor still has a real software path.
            // The capture format is explicitly B8G8R8A8_UNORM.
            std::memset(destination, 16, quality.width * quality.height);
            std::memset(destination + quality.width * quality.height, 128, quality.width * quality.height / 2);
            const double scale = std::min(double(quality.width) / desc.Width, double(quality.height) / desc.Height);
            const UINT w = std::max(2u, UINT(desc.Width * scale) & ~1u);
            const UINT h = std::max(2u, UINT(desc.Height * scale) & ~1u);
            const UINT left = ((quality.width - w) / 2) & ~1u, top = ((quality.height - h) / 2) & ~1u;
            auto rgb = [&](UINT x, UINT y, int& r, int& g, int& b) {
                const auto sx = std::min(desc.Width - 1, UINT(std::uint64_t(x) * desc.Width / w));
                const auto sy = std::min(desc.Height - 1, UINT(std::uint64_t(y) * desc.Height / h));
                const BYTE* pixel = input + sy * mapped.RowPitch + sx * 4;
                b = pixel[0]; g = pixel[1]; r = pixel[2];
            };
            for (UINT y = 0; y < h; y += 2) for (UINT x = 0; x < w; x += 2) {
                int sumR = 0, sumG = 0, sumB = 0;
                for (UINT dy = 0; dy < 2; ++dy) for (UINT dx = 0; dx < 2; ++dx) {
                    int r, g, b; rgb(x + dx, y + dy, r, g, b);
                    sumR += r; sumG += g; sumB += b;
                    destination[(top + y + dy) * quality.width + left + x + dx] = BYTE(std::clamp(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16, 16, 235));
                }
                const int r = sumR / 4, g = sumG / 4, b = sumB / 4;
                BYTE* uv = destination + quality.width * quality.height + ((top + y) / 2) * quality.width + left + x;
                uv[0] = BYTE(std::clamp(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128, 16, 240));
                uv[1] = BYTE(std::clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128, 16, 240));
            }
        }
        check(buffer->SetCurrentLength(bytes), "Set NV12 buffer length");
        return buffer;
    }
};

ComPtr<IMFSample> outputSample(IMFTransform* transform, DWORD stream, bool& supplied) {
    MFT_OUTPUT_STREAM_INFO info{};
    check(transform->GetOutputStreamInfo(stream, &info), "Get transform output information");
    supplied = !(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
    if (!supplied) return {};
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateSample(&sample), "Allocate output sample");
    check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize, 4096), info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Allocate transform output");
    check(sample->AddBuffer(buffer.Get()), "Attach transform output buffer");
    return sample;
}
} // namespace

struct VideoEncoder::Impl {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::optional<GpuFrame> pending;
    std::jthread worker;
    std::atomic<bool> forceKeyframe{true};
    std::atomic<UINT32> requestedBitrate{2500000};
    VideoQuality quality{};
    std::function<void(EncodedVideo)> onFrame;
    std::function<void(std::string)> onError;

    // These members are exclusively accessed by the worker.
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<IMFDXGIDeviceManager> manager;
    Nv12Converter converter;
    bool asynchronous = false, dxgiInput = false;
    unsigned inputCredits = 0, outputCredits = 0;
    DWORD inputStream = 0, outputStream = 0;
    UINT32 activeBitrate = 0;
    std::uint64_t lastSubmittedPts = 0;
    std::vector<std::uint8_t> sps, pps;

    void discardTransform() {
        if (transform) {
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        events.Reset(); transform.Reset(); manager.Reset();
        inputCredits = outputCredits = 0; asynchronous = dxgiInput = false;
        sps.clear(); pps.clear();
    }
    void configure(ComPtr<IMFTransform> candidate, bool hardware) {
        transform = std::move(candidate);
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(&attributes))) {
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            UINT32 value = FALSE;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &value)) && value) {
                check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "Unlock asynchronous encoder");
                check(transform.As(&events), "Query encoder event generator");
                asynchronous = true;
            }
            value = FALSE;
            if (hardware && SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &value)) && value) {
                UINT token = 0;
                check(MFCreateDXGIDeviceManager(&token, &manager), "Create encoder D3D manager");
                check(manager->ResetDevice(converter.device.Get(), token), "Bind encoder D3D device");
                check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())), "Set encoder D3D manager");
                dxgiInput = true;
            }
        }
        DWORD inCount = 0, outCount = 0;
        check(transform->GetStreamCount(&inCount, &outCount), "Get encoder streams");
        if (inCount != 1 || outCount != 1) throw std::runtime_error("Unsupported encoder stream layout");
        if (FAILED(transform->GetStreamIDs(1, &inputStream, 1, &outputStream))) inputStream = outputStream = 0;
        auto output = videoType(MFVideoFormat_H264, quality);
        check(output->SetUINT32(MF_MT_AVG_BITRATE, requestedBitrate), "Set encoder bitrate");
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        output->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4_2);
        check(transform->SetOutputType(outputStream, output.Get(), 0), "Set H264 encoder output");
        auto input = videoType(MFVideoFormat_NV12, quality);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, quality.width);
        input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        check(transform->SetInputType(inputStream, input.Get(), 0), "Set NV12 encoder input");
        setCodec(transform.Get(), CODECAPI_AVEncCommonLowLatency, TRUE, true);
        setCodec(transform.Get(), CODECAPI_AVLowLatencyMode, TRUE);
        setCodec(transform.Get(), CODECAPI_AVEncMPVGOPSize, quality.fps * 2);
        setCodec(transform.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setCodec(transform.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        activeBitrate = requestedBitrate;
        setCodec(transform.Get(), CODECAPI_AVEncCommonMeanBitRate, activeBitrate);
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Start encoder streaming");
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start encoder stream");
    }
    void initialize(const GpuFrame& frame) {
        converter.initialize(frame.texture.Get(), quality);
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        const HRESULT enumerated = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output, &activations, &count);
        bool found = false;
        if (SUCCEEDED(enumerated)) {
            for (UINT32 i = 0; i < count; ++i) {
                if (!found) {
                    ComPtr<IMFTransform> candidate;
                    if (SUCCEEDED(activations[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
                        try { configure(candidate, true); found = true; }
                        catch (...) { discardTransform(); activations[i]->ShutdownObject(); }
                    }
                }
                activations[i]->Release();
            }
            CoTaskMemFree(activations);
        }
        if (!found) {
            ComPtr<IMFTransform> software;
            check(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&software)), "Create software H264 encoder");
            configure(software, false);
        }
        forceKeyframe = true;
    }
    void readEvents() {
        if (!asynchronous) return;
        for (unsigned i = 0; i < 64; ++i) {
            ComPtr<IMFMediaEvent> event;
            const auto hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) break;
            check(hr, "Read encoder event");
            HRESULT status = S_OK; event->GetStatus(&status); check(status, "Encoder event status");
            MediaEventType type = MEUnknown; event->GetType(&type);
            if (type == METransformNeedInput) ++inputCredits;
            else if (type == METransformHaveOutput) ++outputCredits;
            else if (type == MEError) fail(status, "Asynchronous encoder error");
        }
    }
    void captureHeader() {
        ComPtr<IMFMediaType> type;
        if (FAILED(transform->GetOutputCurrentType(outputStream, &type))) return;
        UINT32 length = 0;
        if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length)) || length > 65536) return;
        std::vector<std::uint8_t> header(length);
        if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), length, nullptr))) return;
        auto annex = sequenceAnnexB(header);
        for (auto nal : nals(annex)) {
            if ((nal.front() & 31) == 7) sps.assign(nal.begin(), nal.end());
            if ((nal.front() & 31) == 8) pps.assign(nal.begin(), nal.end());
        }
    }
    void deliver(IMFSample* sample, std::uint64_t fallbackPts) {
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "Get encoded H264 bytes");
        BYTE* data = nullptr; DWORD length = 0;
        check(buffer->Lock(&data, nullptr, &length), "Lock H264 output");
        auto annex = toAnnexB({data, length});
        buffer->Unlock();
        if (annex.empty()) {
            if (length) throw std::runtime_error("Encoder produced unsupported H264 framing");
            return;
        }
        bool idr = false;
        for (auto nal : nals(annex)) {
            const auto kind = nal.front() & 31;
            if (kind == 7) sps.assign(nal.begin(), nal.end());
            if (kind == 8) pps.assign(nal.begin(), nal.end());
            if (kind == 5) idr = true;
        }
        if (idr) {
            captureHeader();
            std::vector<std::uint8_t> prefixed;
            for (const auto* parameter : {&sps, &pps}) if (!parameter->empty()) {
                prefixed.insert(prefixed.end(), {0, 0, 0, 1});
                prefixed.insert(prefixed.end(), parameter->begin(), parameter->end());
            }
            prefixed.insert(prefixed.end(), annex.begin(), annex.end());
            annex = std::move(prefixed);
        }
        LONGLONG pts = 0;
        if (FAILED(sample->GetSampleTime(&pts)) || pts < 0) pts = static_cast<LONGLONG>(fallbackPts * 10);
        if (onFrame) onFrame(EncodedVideo{std::move(annex), static_cast<std::uint64_t>(pts / 10), idr});
    }
    void pull(std::uint64_t fallbackPts) {
        if (asynchronous) readEvents();
        for (unsigned i = 0; i < 16; ++i) {
            if (asynchronous && !outputCredits) break;
            bool supplied = false;
            auto allocated = outputSample(transform.Get(), outputStream, supplied);
            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = outputStream; output.pSample = allocated.Get();
            DWORD status = 0;
            const auto hr = transform->ProcessOutput(0, 1, &output, &status);
            if (asynchronous && outputCredits) --outputCredits;
            if (output.pEvents) output.pEvents->Release();
            ComPtr<IMFSample> sample;
            if (output.pSample && output.pSample != allocated.Get()) sample.Attach(output.pSample);
            else sample = allocated;
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> type;
                check(transform->GetOutputAvailableType(outputStream, 0, &type), "Get changed encoder type");
                check(transform->SetOutputType(outputStream, type.Get(), 0), "Set changed encoder type");
                continue;
            }
            check(hr, "Read encoded H264 sample");
            if (sample) deliver(sample.Get(), fallbackPts);
        }
    }
    void encode(const GpuFrame& frame, std::stop_token stop) {
        if (!transform) initialize(frame);
        D3D11_TEXTURE2D_DESC desc{}; frame.texture->GetDesc(&desc);
        if (desc.Width != converter.sourceWidth || desc.Height != converter.sourceHeight)
            converter.initialize(frame.texture.Get(), quality);
        if (requestedBitrate != activeBitrate) {
            activeBitrate = requestedBitrate;
            setCodec(transform.Get(), CODECAPI_AVEncCommonMeanBitRate, activeBitrate);
        }
        if (forceKeyframe.exchange(false)) setCodec(transform.Get(), CODECAPI_AVEncVideoForceKeyFrame, TRUE);
        // Async encoders only accept input after METransformNeedInput.
        if (asynchronous) {
            const auto deadline = std::chrono::steady_clock::now() + 150ms;
            while (!inputCredits && !stop.stop_requested()) {
                pull(frame.ptsUs);
                if (std::chrono::steady_clock::now() >= deadline) return;
                std::this_thread::sleep_for(1ms);
            }
            if (stop.stop_requested()) return;
        }
        auto nv12 = converter.convert(frame.texture.Get());
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateSample(&sample), "Create video input sample");
        if (dxgiInput && nv12) {
            check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE, &buffer), "Wrap NV12 GPU surface");
            buffer->SetCurrentLength(quality.width * quality.height * 3 / 2);
        } else {
            // D3D-aware transforms may also accept system-memory NV12 samples.
            buffer = converter.cpuBuffer(frame.texture.Get(), nv12.Get());
        }
        check(sample->AddBuffer(buffer.Get()), "Attach NV12 sample buffer");
        check(sample->SetSampleTime(static_cast<LONGLONG>(frame.ptsUs * 10)), "Set video input timestamp");
        check(sample->SetSampleDuration(10000000LL / quality.fps), "Set video input duration");
        HRESULT hr = transform->ProcessInput(inputStream, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) { pull(frame.ptsUs); hr = transform->ProcessInput(inputStream, sample.Get(), 0); }
        if (hr == MF_E_NOTACCEPTING) return; // Drop an old frame rather than grow a queue.
        check(hr, "Submit video to encoder");
        lastSubmittedPts = frame.ptsUs;
        if (asynchronous && inputCredits) --inputCredits;
        pull(frame.ptsUs);
    }
    void run(std::stop_token stop, std::promise<void> ready) {
        bool initialized = false;
        // Keep MF/COM alive while exception cleanup releases every transform.
        std::unique_ptr<ComScope> com;
        std::unique_ptr<MfScope> mf;
        try {
            com = std::make_unique<ComScope>();
            mf = std::make_unique<MfScope>();
            initialized = true; ready.set_value();
            while (!stop.stop_requested()) {
                std::optional<GpuFrame> frame;
                {
                    std::unique_lock lock(mutex);
                    condition.wait_for(lock, stop, 3ms, [&] { return pending.has_value(); });
                    if (pending) { frame = std::move(pending); pending.reset(); }
                }
                if (stop.stop_requested()) break;
                if (frame) encode(*frame, stop);
                else if (transform && asynchronous) pull(lastSubmittedPts);
            }
            discardTransform();
            converter = Nv12Converter{};
        } catch (const std::exception& error) {
            discardTransform();
            converter = Nv12Converter{};
            if (!initialized) ready.set_exception(std::current_exception());
            else if (onError && !stop.stop_requested()) { try { onError(error.what()); } catch (...) {} }
        }
    }
};

VideoEncoder::VideoEncoder() : impl_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() { stop(); }
void VideoEncoder::start(VideoQuality quality, std::function<void(EncodedVideo)> onFrame,
                         std::function<void(std::string)> onError) {
    stop();
    if ((quality.width != 1280 || quality.height != 720) && (quality.width != 1920 || quality.height != 1080))
        throw std::invalid_argument("Video resolution must be 1280x720 or 1920x1080");
    if (quality.fps != 30 && quality.fps != 60) throw std::invalid_argument("Video rate must be 30 or 60 FPS");
    impl_->quality = quality; impl_->requestedBitrate = quality.bitrate;
    impl_->onFrame = std::move(onFrame); impl_->onError = std::move(onError);
    std::promise<void> ready; auto future = ready.get_future();
    impl_->worker = std::jthread([this, ready = std::move(ready)](std::stop_token stop) mutable { impl_->run(stop, std::move(ready)); });
    try { future.get(); } catch (...) { stop(); throw; }
}
void VideoEncoder::submit(GpuFrame frame) {
    if (!frame.texture) return;
    { std::lock_guard lock(impl_->mutex); impl_->pending = std::move(frame); }
    impl_->condition.notify_one();
}
void VideoEncoder::requestKeyframe() { impl_->forceKeyframe = true; }
void VideoEncoder::setBitrate(std::uint32_t bitrate) { impl_->requestedBitrate = std::clamp(bitrate, 250000u, 12000000u); }
void VideoEncoder::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop(); impl_->condition.notify_all(); impl_->worker.join();
    }
    std::lock_guard lock(impl_->mutex); impl_->pending.reset();
}

struct VideoDecoder::Impl {
    std::mutex mutex;
    ComPtr<IMFTransform> transform;
    VideoQuality dimensions{};
    bool started = false, bt709 = true;
    LONG stride = 0;
    UINT displayWidth = 0, displayHeight = 0, offsetX = 0, offsetY = 0;
    // The MF decoder may normalize output sample times (including returning
    // zero), while the call needs the common sender RTP timeline. Each input
    // here is one complete H264 access unit; our Baseline encoder disables B
    // pictures, so decoded pictures retain input order. Keep the source PTS
    // until ProcessOutput actually emits its corresponding picture.
    std::deque<std::uint64_t> inputPts;
    MfScope mf;

    ~Impl() {
        // mf is the last member and would otherwise shut down before transform.
        try { ComScope com; transform.Reset(); } catch (...) { transform.Reset(); }
    }
    void initialize() {
        inputPts.clear();
        check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)), "Create H264 decoder");
        setCodec(transform.Get(), CODECAPI_AVLowLatencyMode, TRUE);
        auto input = videoType(MFVideoFormat_H264, VideoQuality{1920, 1080, 30, 4000000});
        check(transform->SetInputType(0, input.Get(), 0), "Set H264 decoder input");
        chooseOutput();
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Start decoder streaming");
        check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start decoder stream");
        started = true;
    }
    void chooseOutput() {
        for (DWORD index = 0; ; ++index) {
            ComPtr<IMFMediaType> type;
            const HRESULT hr = transform->GetOutputAvailableType(0, index, &type);
            if (hr == MF_E_NO_MORE_TYPES) throw std::runtime_error("H264 decoder has no NV12 output");
            check(hr, "Enumerate decoder output types");
            GUID subtype{}; type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            check(transform->SetOutputType(0, type.Get(), 0), "Set decoder NV12 output");
            check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &dimensions.width, &dimensions.height), "Get decoded frame size");
            if (!dimensions.width || !dimensions.height || dimensions.width > 8192 || dimensions.height > 8192)
                throw std::runtime_error("Invalid or excessive decoded video dimensions");
            UINT32 value = 0;
            stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &value)) ? static_cast<LONG>(value) : static_cast<LONG>(dimensions.width);
            UINT32 matrix = MFVideoTransferMatrix_BT709;
            type->GetUINT32(MF_MT_YUV_MATRIX, &matrix); bt709 = matrix == MFVideoTransferMatrix_BT709;
            displayWidth = dimensions.width; displayHeight = dimensions.height;
            offsetX = offsetY = 0;
            // H264 coded dimensions may include bottom padding (1088 for 1080p).
            MFVideoArea aperture{};
            HRESULT apertureHr = type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), nullptr);
            if (FAILED(apertureHr)) apertureHr = type->GetBlob(MF_MT_GEOMETRIC_APERTURE,
                reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), nullptr);
            if (SUCCEEDED(apertureHr) && aperture.Area.cx > 0 && aperture.Area.cy > 0 &&
                aperture.OffsetX.value >= 0 && aperture.OffsetY.value >= 0 &&
                UINT(aperture.OffsetX.value) + UINT(aperture.Area.cx) <= dimensions.width &&
                UINT(aperture.OffsetY.value) + UINT(aperture.Area.cy) <= dimensions.height) {
                offsetX = UINT(aperture.OffsetX.value); offsetY = UINT(aperture.OffsetY.value);
                displayWidth = UINT(aperture.Area.cx); displayHeight = UINT(aperture.Area.cy);
            }
            return;
        }
    }
    std::uint64_t picturePts(IMFSample* sample) {
        if (inputPts.empty())
            throw std::runtime_error("Decoder emitted a picture without a corresponding source timestamp");
        auto chosen = inputPts.begin();
        LONGLONG reported = 0;
        if (SUCCEEDED(sample->GetSampleTime(&reported)) && reported >= 0) {
            const auto source = static_cast<std::uint64_t>(reported / 10);
            const auto matched = std::find(inputPts.begin(), inputPts.end(), source);
            if (matched != inputPts.end()) chosen = matched;
        }
        const auto sourcePts = *chosen;
        // An exact later timestamp means MF dropped older queued pictures.
        // Otherwise the FIFO associates normalized/absent MF clocks correctly.
        inputPts.erase(inputPts.begin(), std::next(chosen));
        return sourcePts;
    }
    VideoFrame convert(IMFSample* sample, std::uint64_t sourcePts) {
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "Get decoded NV12 bytes");
        BYTE* data = nullptr; DWORD length = 0;
        check(buffer->Lock(&data, nullptr, &length), "Lock decoded frame");
        struct Unlock { IMFMediaBuffer* buffer; ~Unlock() { buffer->Unlock(); } } unlock{buffer.Get()};
        const auto pitch = static_cast<UINT>(std::abs(stride));
        if (pitch < dimensions.width || length < std::uint64_t(pitch) * dimensions.height * 3 / 2)
            throw std::runtime_error("Decoder returned a truncated NV12 image");
        VideoFrame result;
        result.width = displayWidth; result.height = displayHeight; result.stride = result.width * 4;
        result.bgra.resize(std::size_t(result.stride) * result.height);
        result.ptsUs = sourcePts;
        const BYTE* uv = data + std::size_t(pitch) * dimensions.height;
        for (UINT y = 0; y < result.height; ++y) {
            const UINT sourceY = y + offsetY;
            const BYTE* yRow = data + std::size_t(stride >= 0 ? sourceY : dimensions.height - 1 - sourceY) * pitch;
            const BYTE* uvRow = uv + std::size_t(sourceY / 2) * pitch;
            BYTE* dest = result.bgra.data() + std::size_t(y) * result.stride;
            for (UINT x = 0; x < result.width; ++x) {
                const UINT sourceX = x + offsetX;
                const int yy = std::max(0, int(yRow[sourceX]) - 16), u = int(uvRow[sourceX & ~1u]) - 128, v = int(uvRow[(sourceX & ~1u) + 1]) - 128;
                const int r = (298 * yy + (bt709 ? 459 : 409) * v + 128) >> 8;
                const int g = (298 * yy - (bt709 ? 55 : 100) * u - (bt709 ? 136 : 208) * v + 128) >> 8;
                const int b = (298 * yy + (bt709 ? 541 : 516) * u + 128) >> 8;
                dest[x * 4] = BYTE(std::clamp(b, 0, 255)); dest[x * 4 + 1] = BYTE(std::clamp(g, 0, 255));
                dest[x * 4 + 2] = BYTE(std::clamp(r, 0, 255)); dest[x * 4 + 3] = 255;
            }
        }
        return result;
    }
    std::optional<VideoFrame> pull() {
        std::optional<VideoFrame> latest;
        for (unsigned iteration = 0; iteration < 8; ++iteration) {
            bool supplied = false;
            auto allocated = outputSample(transform.Get(), 0, supplied);
            MFT_OUTPUT_DATA_BUFFER output{}; output.pSample = allocated.Get();
            DWORD status = 0;
            const HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            ComPtr<IMFSample> sample;
            if (output.pSample && output.pSample != allocated.Get()) sample.Attach(output.pSample);
            else sample = allocated;
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { chooseOutput(); continue; }
            check(hr, "Decode H264 picture");
            if (sample) latest = convert(sample.Get(), picturePts(sample.Get()));
        }
        return latest;
    }
};
VideoDecoder::VideoDecoder() : impl_(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;
std::optional<VideoFrame> VideoDecoder::decode(std::span<const std::uint8_t> annexB, std::uint64_t ptsUs) {
    if (annexB.empty()) return {};
    if (annexB.size() > 16 * 1024 * 1024) throw std::runtime_error("H264 picture exceeds safety limit");
    std::lock_guard lock(impl_->mutex);
    ComScope com;
    if (!impl_->transform) impl_->initialize();
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    check(MFCreateMemoryBuffer(static_cast<DWORD>(annexB.size()), &buffer), "Allocate H264 input");
    BYTE* data = nullptr;
    check(buffer->Lock(&data, nullptr, nullptr), "Lock H264 input");
    std::memcpy(data, annexB.data(), annexB.size()); buffer->Unlock();
    check(buffer->SetCurrentLength(static_cast<DWORD>(annexB.size())), "Set H264 length");
    check(MFCreateSample(&sample), "Create H264 sample");
    check(sample->AddBuffer(buffer.Get()), "Attach H264 input");
    sample->SetSampleTime(static_cast<LONGLONG>(ptsUs * 10));
    sample->SetSampleDuration(10000000LL / 30);
    HRESULT hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
    std::optional<VideoFrame> earlier;
    if (hr == MF_E_NOTACCEPTING) {
        earlier = impl_->pull();
        hr = impl_->transform->ProcessInput(0, sample.Get(), 0);
    }
    check(hr, "Submit H264 picture");
    const auto units = nals(annexB);
    const bool picture = std::any_of(units.begin(), units.end(), [](const auto& nal) {
        const auto type = nal.front() & 31;
        return type >= 1 && type <= 5;
    });
    if (picture) {
        if (impl_->inputPts.size() >= 32)
            throw std::runtime_error("H264 decoder exceeded the bounded source timestamp queue");
        impl_->inputPts.push_back(ptsUs);
    }
    auto latest = impl_->pull();
    return latest ? std::move(latest) : std::move(earlier);
}
void VideoDecoder::reset() {
    std::lock_guard lock(impl_->mutex);
    ComScope com;
    impl_->inputPts.clear();
    if (impl_->transform) {
        impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        impl_->transform.Reset();
    }
}
} // namespace nativecall
