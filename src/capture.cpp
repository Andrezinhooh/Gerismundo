#include "nativecall/capture.hpp"

#include <Windows.h>
#include <d3d10.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace nativecall {
namespace {

using Microsoft::WRL::ComPtr;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::SizeInt32;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

std::string exceptionText() {
    try {
        throw;
    } catch (const winrt::hresult_error& error) {
        return winrt::to_string(error.message());
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Falha desconhecida na captura do Windows.";
    }
}

std::wstring executableName(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"Aplicativo";
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    CloseHandle(process);
    if (!ok) return L"Aplicativo";
    path.resize(size);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

BOOL CALLBACK enumerateMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& sources = *reinterpret_cast<std::vector<CaptureSource>*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    const auto width = info.rcMonitor.right - info.rcMonitor.left;
    const auto height = info.rcMonitor.bottom - info.rcMonitor.top;
    std::wstring name = L"Tela " + std::to_wstring(sources.size() + 1) + L" - ";
    name += info.szDevice;
    name += L" (" + std::to_wstring(width) + L" x " + std::to_wstring(height) + L")";
    if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0) name += L" - Principal";
    sources.push_back({SourceKind::Monitor, reinterpret_cast<std::uintptr_t>(monitor), 0,
                       std::move(name)});
    return TRUE;
}

BOOL CALLBACK enumerateWindow(HWND window, LPARAM parameter) {
    auto& sources = *reinterpret_cast<std::vector<CaptureSource>*>(parameter);
    if (!IsWindowVisible(window)) return TRUE;
    DWORD pid{};
    if (GetWindowThreadProcessId(window, &pid) == 0 || pid == GetCurrentProcessId()) return TRUE;
    const auto style = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_STYLE));
    const auto extended = static_cast<DWORD_PTR>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if ((style & WS_CHILD) != 0) return TRUE;
    if ((extended & WS_EX_TOOLWINDOW) != 0 && (extended & WS_EX_APPWINDOW) == 0) return TRUE;
    DWORD cloaked{};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;
    DWORD affinity{};
    if (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE) return TRUE;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return TRUE;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return TRUE;
    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0) return TRUE;
    title.resize(static_cast<std::size_t>(copied));
    auto name = executableName(pid) + L" - " + title + L" [PID " + std::to_wstring(pid) + L"]";
    sources.push_back({SourceKind::Window, reinterpret_cast<std::uintptr_t>(window), pid,
                       std::move(name)});
    return TRUE;
}

struct CaptureRun final : std::enable_shared_from_this<CaptureRun> {
    CaptureSource source;
    VideoQuality quality;
    ScreenCapture::FrameCallback onFrame;
    ScreenCapture::ErrorCallback onError;
    std::atomic<bool> stopping{false};
    std::mutex queueMutex;
    std::condition_variable condition;
    std::optional<GpuFrame> pending;
    std::string failure;
    std::mutex captureMutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    IDirect3DDevice directDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    winrt::event_token arrivedToken{};
    winrt::event_token closedToken{};
    bool arrivedRegistered{};
    bool closedRegistered{};
    SizeInt32 size{};
    std::uint64_t lastFrameUs{};

    void requestStop() noexcept {
        stopping.store(true);
        condition.notify_all();
    }

    void fail(std::string error) noexcept {
        {
            std::lock_guard lock(queueMutex);
            if (failure.empty() && !stopping.load()) failure = std::move(error);
            stopping.store(true);
        }
        condition.notify_all();
    }

    void initialize() {
        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (!ApiInformation::IsMethodPresent(
                L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", L"CreateFreeThreaded") ||
            !GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows Graphics Capture nao esta disponivel neste sistema.");
        }
        if (source.kind == SourceKind::Window) {
            const auto window = reinterpret_cast<HWND>(source.handle);
            DWORD pid{};
            if (!IsWindow(window) || GetWindowThreadProcessId(window, &pid) == 0 ||
                pid != source.processId || pid == GetCurrentProcessId()) {
                throw std::runtime_error("A janela selecionada foi fechada ou nao pode ser capturada.");
            }
        } else {
            MONITORINFO monitorInfo{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(source.handle), &monitorInfo))
                throw std::runtime_error("O monitor selecionado foi desconectado.");
        }

        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                           levels, 2, D3D11_SDK_VERSION,
                                           device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (FAILED(result)) {
            device.Reset();
            context.Reset();
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                      D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                      D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                                      context.GetAddressOf());
        }
        winrt::check_hresult(result);
        // The encoder uses this device/context from its own worker. Let the
        // runtime serialize immediate/video-context access with capture copies.
        ComPtr<ID3D10Multithread> multithread;
        winrt::check_hresult(context.As(&multithread));
        multithread->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        directDevice = inspectable.as<IDirect3DDevice>();
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (source.kind == SourceKind::Monitor) {
            winrt::check_hresult(factory->CreateForMonitor(reinterpret_cast<HMONITOR>(source.handle),
                winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        } else {
            winrt::check_hresult(factory->CreateForWindow(reinterpret_cast<HWND>(source.handle),
                winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        }
        size = item.Size();
        if (size.Width <= 0 || size.Height <= 0)
            throw std::runtime_error("A fonte selecionada nao tem uma area de captura valida.");
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            directDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        const std::weak_ptr<CaptureRun> weak = shared_from_this();
        arrivedToken = pool.FrameArrived([weak](auto const&, auto const&) noexcept {
            if (auto run = weak.lock()) run->frameArrived();
        });
        arrivedRegistered = true;
        closedToken = item.Closed([weak](auto const&, auto const&) noexcept {
            if (auto run = weak.lock()) run->fail("A fonte compartilhada foi fechada ou desconectada.");
        });
        closedRegistered = true;
        session = pool.CreateCaptureSession(item);
        session.StartCapture();
    }

    void frameArrived() noexcept {
        if (stopping.load()) return;
        std::lock_guard captureLock(captureMutex);
        if (stopping.load()) return;
        try {
            auto frame = pool.TryGetNextFrame();
            if (!frame) return;
            const auto content = frame.ContentSize();
            if (content.Width <= 0 || content.Height <= 0) return;
            if (content.Width != size.Width || content.Height != size.Height) {
                frame.Close();
                size = content;
                pool.Recreate(directDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                return;
            }
            const auto now = mediaNowUs();
            const auto interval = 1'000'000ULL / (std::max)(1U, quality.fps);
            if (lastFrameUs != 0 && now - lastFrameUs < interval) return;
            auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> sourceTexture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(sourceTexture.GetAddressOf())));
            D3D11_TEXTURE2D_DESC sourceDescription{};
            sourceTexture->GetDesc(&sourceDescription);
            D3D11_TEXTURE2D_DESC copyDescription{};
            copyDescription.Width = (std::min)(sourceDescription.Width, static_cast<UINT>(content.Width));
            copyDescription.Height = (std::min)(sourceDescription.Height, static_cast<UINT>(content.Height));
            copyDescription.MipLevels = 1;
            copyDescription.ArraySize = 1;
            copyDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            copyDescription.SampleDesc.Count = 1;
            copyDescription.Usage = D3D11_USAGE_DEFAULT;
            copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            GpuFrame copy;
            winrt::check_hresult(device->CreateTexture2D(&copyDescription, nullptr,
                                                        copy.texture.GetAddressOf()));
            const D3D11_BOX box{0, 0, 0, copyDescription.Width, copyDescription.Height, 1};
            context->CopySubresourceRegion(copy.texture.Get(), 0, 0, 0, 0, sourceTexture.Get(), 0, &box);
            winrt::check_hresult(device->GetDeviceRemovedReason());
            copy.ptsUs = now;
            copy.width = copyDescription.Width;
            copy.height = copyDescription.Height;
            lastFrameUs = now;
            frame.Close();
            {
                std::lock_guard queueLock(queueMutex);
                if (!stopping.load()) pending = std::move(copy);
            }
            condition.notify_one();
        } catch (...) {
            fail("Falha na captura de video: " + exceptionText());
        }
    }

    void close() noexcept {
        // Mark first: callbacks already queued by WinRT become harmless.
        stopping.store(true);
        try { if (arrivedRegistered && pool) pool.FrameArrived(arrivedToken); } catch (...) {}
        try { if (closedRegistered && item) item.Closed(closedToken); } catch (...) {}
        {
            // Drain an event that entered before stopping was set. Do not hold
            // this mutex while Close waits for the frame-pool event machinery.
            std::lock_guard captureLock(captureMutex);
        }
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        session = nullptr;
        pool = nullptr;
        item = nullptr;
        directDevice = nullptr;
        context.Reset();
        device.Reset();
        std::lock_guard queueLock(queueMutex);
        pending.reset();
    }

    void run() noexcept {
        bool apartmentInitialized = false;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartmentInitialized = true;
            initialize();
            for (;;) {
                std::optional<GpuFrame> next;
                {
                    std::unique_lock lock(queueMutex);
                    condition.wait(lock, [this] { return stopping.load() || pending.has_value(); });
                    if (stopping.load()) break;
                    next.swap(pending);
                }
                if (next && onFrame) onFrame(std::move(*next));
            }
        } catch (...) {
            fail("Nao foi possivel iniciar/continuar o compartilhamento: " + exceptionText());
        }
        close();
        std::string error;
        {
            std::lock_guard lock(queueMutex);
            error = std::move(failure);
        }
        if (!error.empty() && onError) {
            try { onError(std::move(error)); } catch (...) {}
        }
        if (apartmentInitialized) winrt::uninit_apartment();
    }
};

} // namespace

struct ScreenCapture::Impl {
    std::shared_ptr<CaptureRun> run;
    std::thread thread;

    void stop() {
        if (run) run->requestStop();
        if (thread.joinable()) {
            // A callback may stop its own capture. Its run object stays alive in
            // the worker closure until cleanup completes, without self-joining.
            if (thread.get_id() == std::this_thread::get_id()) thread.detach();
            else thread.join();
        }
        run.reset();
    }
};

ScreenCapture::ScreenCapture() : impl_(std::make_unique<Impl>()) {}
ScreenCapture::~ScreenCapture() { stop(); }

void ScreenCapture::start(const CaptureSource& source, const VideoQuality& quality,
                          FrameCallback onFrame, ErrorCallback onError) {
    stop();
    if (!onFrame) throw std::invalid_argument("ScreenCapture requires a frame callback.");
    if (quality.fps != 30 && quality.fps != 60)
        throw std::invalid_argument("A taxa de captura deve ser 30 ou 60 FPS.");
    auto run = std::make_shared<CaptureRun>();
    run->source = source;
    run->quality = quality;
    run->onFrame = std::move(onFrame);
    run->onError = std::move(onError);
    impl_->run = run;
    impl_->thread = std::thread([run = std::move(run)] { run->run(); });
}

void ScreenCapture::stop() { impl_->stop(); }

std::vector<CaptureSource> ScreenCapture::enumerateSources(SourceKind kind) {
    std::vector<CaptureSource> sources;
    if (kind == SourceKind::Monitor)
        EnumDisplayMonitors(nullptr, nullptr, enumerateMonitor, reinterpret_cast<LPARAM>(&sources));
    else
        EnumWindows(enumerateWindow, reinterpret_cast<LPARAM>(&sources));
    return sources;
}

} // namespace nativecall
