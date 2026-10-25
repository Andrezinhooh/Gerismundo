#include "nativecall/ui.hpp"
#include "nativecall/audio.hpp"
#include "nativecall/capture.hpp"
#include "nativecall/controller.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace nativecall {
namespace {

constexpr UINT UpdateMessage = WM_APP + 1;
constexpr UINT ErrorMessage = WM_APP + 2;
constexpr UINT FullscreenMessage = WM_APP + 3;
constexpr int MaxRows = 4;
constexpr wchar_t MainClass[] = L"NativeCallMain";
constexpr wchar_t VideoClass[] = L"NativeCallVideo";
constexpr wchar_t ShareClass[] = L"NativeCallShare";
constexpr wchar_t DeviceClass[] = L"NativeCallDevices";

enum Control : int {
    Server = 100, Room, Name, Token, Join, Leave, Microphone, Deafen,
    Devices, Share, StopShare, MuteShare, Fullscreen, Status,
    RowBase = 500, SourceTab = 1000, SourceScroll, Quality, Fps, WithAudio,
    StartTransmission, CancelDialog, InputDevice, OutputDevice, ApplyDevices
};

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    if (!count) return L"Texto inválido";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("O texto contém caracteres inválidos.");
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring windowText(HWND window) {
    int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    length = GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void setText(HWND window, const std::wstring& value) {
    if (windowText(window) != value) SetWindowTextW(window, value.c_str());
}

int scaled(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }

HWND control(HWND parent, HINSTANCE instance, HFONT font, const wchar_t* className,
             const wchar_t* title, DWORD style, int id, DWORD extendedStyle = 0) {
    const HWND window = CreateWindowExW(extendedStyle, className, title,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (!window) throw std::runtime_error("Não foi possível criar um controle da interface.");
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetWindowTheme(window, L"Explorer", nullptr);
    return window;
}

void messageError(HWND parent, const std::string& error) {
    const HWND popup = parent ? GetLastActivePopup(parent) : nullptr;
    if (popup && IsWindowVisible(popup)) parent = popup;
    MessageBoxW(parent, wide(error).c_str(), L"Native Call", MB_OK | MB_ICONWARNING);
}

void modalLoop(HWND window, HWND owner) {
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (IsWindow(window)) {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (!result) PostQuitMessage(static_cast<int>(message.wParam));
            if (IsWindow(window)) DestroyWindow(window);
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

void centerOverOwner(HWND window, HWND owner) {
    RECT a{}, b{};
    GetWindowRect(owner, &a);
    GetWindowRect(window, &b);
    const auto monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    int x = a.left + (a.right - a.left - b.right + b.left) / 2;
    int y = a.top + (a.bottom - a.top - b.bottom + b.top) / 2;
    x = std::max(static_cast<int>(info.rcWork.left),
                 std::min(x, static_cast<int>(info.rcWork.right - (b.right - b.left))));
    y = std::max(static_cast<int>(info.rcWork.top),
                 std::min(y, static_cast<int>(info.rcWork.bottom - (b.bottom - b.top))));
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

struct ShareChoice { CaptureSource source; VideoQuality quality; bool audio{true}; };

class SharePicker {
public:
    SharePicker(HINSTANCE instance, HWND owner, HFONT font, UINT dpi)
        : instance_(instance), owner_(owner), font_(font), dpi_(dpi) {}
    ~SharePicker() { releaseSources(); }

    std::optional<ShareChoice> show() {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = procedure;
        cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        cls.lpszClassName = ShareClass;
        RegisterClassExW(&cls);
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, ShareClass,
            L"Escolha o que deseja compartilhar", WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, d(900), d(700), owner_, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("Não foi possível abrir o seletor de compartilhamento.");
        centerOverOwner(window_, owner_);
        modalLoop(window_, owner_);
        return result_;
    }

private:
    struct SourceView {
        CaptureSource source;
        HTHUMBNAIL thumbnail{};
        HBITMAP bitmap{};
        HICON icon{};
        SIZE dimensions{};
    };
    HINSTANCE instance_{};
    HWND owner_{}, window_{}, tabs_{}, scrollbar_{}, quality_{}, fps_{}, audio_{}, start_{}, warning_{};
    HFONT font_{};
    UINT dpi_{96};
    RECT sourceArea_{};
    std::vector<SourceView> sources_;
    std::vector<HWND> labels_;
    int selected_{-1}, offset_{};
    std::optional<ShareChoice> result_;

    int d(int value) const { return scaled(value, dpi_); }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<SharePicker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<SharePicker*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->handle(message, wParam, lParam); }
        catch (const std::exception& exception) {
            messageError(window, exception.what());
            return message == WM_CREATE ? -1 : 0;
        }
    }

    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return control(window_, instance_, font_, cls, text, style, id);
    }

    void create() {
        tabs_ = add(WC_TABCONTROLW, L"", WS_TABSTOP, SourceTab);
        TCITEMW item{}; item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(L"Telas"); TabCtrl_InsertItem(tabs_, 0, &item);
        item.pszText = const_cast<wchar_t*>(L"Aplicativos"); TabCtrl_InsertItem(tabs_, 1, &item);
        scrollbar_ = add(L"SCROLLBAR", L"", SBS_VERT, SourceScroll);
        labels_.push_back(add(L"STATIC", L"Qualidade", SS_LEFT, 0));
        labels_.push_back(add(L"STATIC", L"FPS", SS_LEFT, 0));
        quality_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Quality);
        SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"720p"));
        SendMessageW(quality_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1080p"));
        SendMessageW(quality_, CB_SETCURSEL, 0, 0);
        fps_ = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Fps);
        SendMessageW(fps_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30"));
        SendMessageW(fps_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60"));
        SendMessageW(fps_, CB_SETCURSEL, 0, 0);
        audio_ = add(L"BUTTON", L"Compartilhar áudio", BS_AUTOCHECKBOX | WS_TABSTOP, WithAudio);
        SendMessageW(audio_, BM_SETCHECK, BST_CHECKED, 0);
        warning_ = add(L"STATIC",
            L"Tela inteira: o áudio deste aplicativo e de seus processos filhos será excluído.\r\n"
            L"Aplicativos: somente a árvore do processo escolhido; navegadores podem ter áudio fora dela.\r\n"
            L"Ao selecionar outro aplicativo de comunicação, o áudio dele também poderá ser transmitido.", SS_LEFT, 0);
        if (!AudioEngine::selectiveLoopbackSupported()) {
            SendMessageW(audio_, BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(audio_, FALSE);
            setText(audio_, L"Áudio indisponível neste Windows");
            setText(warning_,
                L"Captura seletiva de áudio indisponível: requer Windows build 20348 ou posterior.\r\n"
                L"A transmissão usará somente imagem; não será capturado o áudio completo do sistema.\r\n"
                L"Em Windows compatível, a captura de aplicativos inclui somente a árvore do processo escolhido.");
        }
        start_ = add(L"BUTTON", L"Iniciar transmissão", BS_DEFPUSHBUTTON | WS_TABSTOP, StartTransmission);
        add(L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP, CancelDialog);
        EnableWindow(start_, FALSE);
        layout();
        refresh();
    }

    void layout() {
        RECT client{}; GetClientRect(window_, &client);
        const int bottom = client.bottom - d(205);
        MoveWindow(tabs_, d(16), d(12), client.right - d(32), d(34), TRUE);
        sourceArea_ = {d(16), d(58), client.right - d(40), bottom - d(12)};
        MoveWindow(scrollbar_, client.right - d(34), sourceArea_.top, d(18),
                   sourceArea_.bottom - sourceArea_.top, TRUE);
        MoveWindow(labels_[0], d(20), bottom + d(4), d(75), d(22), TRUE);
        MoveWindow(quality_, d(100), bottom, d(120), d(140), TRUE);
        MoveWindow(labels_[1], d(255), bottom + d(4), d(45), d(22), TRUE);
        MoveWindow(fps_, d(305), bottom, d(90), d(140), TRUE);
        MoveWindow(audio_, d(435), bottom, d(240), d(28), TRUE);
        MoveWindow(warning_, d(20), bottom + d(45), client.right - d(40), d(82), TRUE);
        MoveWindow(start_, client.right - d(340), client.bottom - d(52), d(190), d(32), TRUE);
        MoveWindow(GetDlgItem(window_, CancelDialog), client.right - d(135), client.bottom - d(52), d(115), d(32), TRUE);
        updateScroll();
    }

    void releaseSources() {
        for (auto& view : sources_) {
            if (view.thumbnail) DwmUnregisterThumbnail(view.thumbnail);
            if (view.bitmap) DeleteObject(view.bitmap);
            if (view.icon) DestroyIcon(view.icon);
        }
        sources_.clear();
    }

    HBITMAP monitorPreview(HMONITOR monitor) {
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) return nullptr;
        HDC desktop = GetDC(nullptr);
        if (!desktop) return nullptr;
        HDC memory = CreateCompatibleDC(desktop);
        if (!memory) { ReleaseDC(nullptr, desktop); return nullptr; }
        HBITMAP bitmap = CreateCompatibleBitmap(desktop, d(160), d(90));
        if (!bitmap) { DeleteDC(memory); ReleaseDC(nullptr, desktop); return nullptr; }
        auto old = SelectObject(memory, bitmap);
        RECT area{0, 0, d(160), d(90)};
        FillRect(memory, &area, GetSysColorBrush(COLOR_WINDOWTEXT));
        const int w = info.rcMonitor.right - info.rcMonitor.left;
        const int h = info.rcMonitor.bottom - info.rcMonitor.top;
        const double ratio = std::min(static_cast<double>(d(160)) / w, static_cast<double>(d(90)) / h);
        const int targetW = std::max(1, static_cast<int>(w * ratio));
        const int targetH = std::max(1, static_cast<int>(h * ratio));
        SetStretchBltMode(memory, HALFTONE);
        const BOOL okay = StretchBlt(memory, (d(160) - targetW) / 2, (d(90) - targetH) / 2,
            targetW, targetH, desktop, info.rcMonitor.left, info.rcMonitor.top, w, h, SRCCOPY | CAPTUREBLT);
        SelectObject(memory, old);
        DeleteDC(memory);
        ReleaseDC(nullptr, desktop);
        if (!okay) { DeleteObject(bitmap); return nullptr; }
        return bitmap;
    }

    void refresh() {
        releaseSources();
        offset_ = 0; selected_ = -1;
        const auto kind = TabCtrl_GetCurSel(tabs_) == 1 ? SourceKind::Window : SourceKind::Monitor;
        for (const auto& source : ScreenCapture::enumerateSources(kind)) {
            SourceView view; view.source = source;
            if (kind == SourceKind::Window) {
                const auto sourceWindow = reinterpret_cast<HWND>(source.handle);
                if (FAILED(DwmRegisterThumbnail(window_, sourceWindow, &view.thumbnail))) view.thumbnail = nullptr;
                if (view.thumbnail) DwmQueryThumbnailSourceSize(view.thumbnail, &view.dimensions);
                HICON icon{};
                DWORD_PTR iconResult{};
                if (SendMessageTimeoutW(sourceWindow, WM_GETICON, ICON_SMALL2, 0,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 70, &iconResult))
                    icon = reinterpret_cast<HICON>(iconResult);
                else icon = nullptr;
                if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(sourceWindow, GCLP_HICONSM));
                view.icon = CopyIcon(icon ? icon : LoadIconW(nullptr, IDI_APPLICATION));
            } else {
                view.bitmap = monitorPreview(reinterpret_cast<HMONITOR>(source.handle));
                view.icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
            }
            sources_.push_back(std::move(view));
        }
        EnableWindow(start_, FALSE);
        updateScroll();
        InvalidateRect(window_, nullptr, TRUE);
    }

    RECT card(int index) const {
        const int y = sourceArea_.top + index * d(110) - offset_;
        return {sourceArea_.left, y, sourceArea_.right, y + d(104)};
    }

    RECT preview(int index) const {
        auto area = card(index);
        return {area.left + d(8), area.top + d(7), area.left + d(168), area.top + d(97)};
    }

    void updateScroll() {
        if (!scrollbar_) return;
        const int height = std::max(1L, sourceArea_.bottom - sourceArea_.top);
        const int maxOffset = std::max(0, static_cast<int>(sources_.size()) * d(110) - height);
        offset_ = std::clamp(offset_, 0, maxOffset);
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
        scroll.nMin = 0; scroll.nMax = std::max(0, static_cast<int>(sources_.size()) * d(110) - 1);
        scroll.nPage = static_cast<UINT>(height); scroll.nPos = offset_;
        SetScrollInfo(scrollbar_, SB_CTL, &scroll, TRUE);
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            auto& source = sources_[index];
            if (!source.thumbnail) continue;
            RECT destination = preview(static_cast<int>(index));
            const bool visible = destination.top >= sourceArea_.top && destination.bottom <= sourceArea_.bottom;
            if (source.dimensions.cx > 0 && source.dimensions.cy > 0) {
                const double ratio = std::min(static_cast<double>(destination.right - destination.left) / source.dimensions.cx,
                                              static_cast<double>(destination.bottom - destination.top) / source.dimensions.cy);
                const LONG width = static_cast<LONG>(source.dimensions.cx * ratio);
                const LONG height2 = static_cast<LONG>(source.dimensions.cy * ratio);
                const LONG centerX = (destination.right + destination.left) / 2;
                const LONG centerY = (destination.bottom + destination.top) / 2;
                destination = {centerX - width / 2, centerY - height2 / 2,
                               centerX - width / 2 + width, centerY - height2 / 2 + height2};
            }
            DWM_THUMBNAIL_PROPERTIES props{};
            props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            props.rcDestination = destination; props.fVisible = visible; props.opacity = 255;
            props.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(source.thumbnail, &props);
        }
    }

    void paint() {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        auto oldFont = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);
        SaveDC(dc);
        IntersectClipRect(dc, sourceArea_.left, sourceArea_.top, sourceArea_.right, sourceArea_.bottom);
        if (sources_.empty()) {
            RECT area = sourceArea_;
            DrawTextW(dc, L"Nenhuma fonte elegível encontrada.", -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            auto& view = sources_[index];
            RECT area = card(static_cast<int>(index));
            if (area.bottom < sourceArea_.top || area.top > sourceArea_.bottom) continue;
            FillRect(dc, &area, GetSysColorBrush(selected_ == static_cast<int>(index) ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
            SetTextColor(dc, GetSysColor(selected_ == static_cast<int>(index) ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            RECT image = preview(static_cast<int>(index));
            FillRect(dc, &image, GetSysColorBrush(COLOR_WINDOWTEXT));
            if (view.bitmap) {
                HDC memory = CreateCompatibleDC(dc);
                auto old = SelectObject(memory, view.bitmap);
                BitBlt(dc, image.left, image.top, image.right - image.left, image.bottom - image.top, memory, 0, 0, SRCCOPY);
                SelectObject(memory, old); DeleteDC(memory);
            } else if (!view.thumbnail) {
                const auto oldColor = SetTextColor(dc, RGB(230, 230, 230));
                DrawTextW(dc, L"Prévia indisponível", -1, &image, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SetTextColor(dc, oldColor);
            }
            if (view.icon) DrawIconEx(dc, area.left + d(182), area.top + d(12), view.icon, d(24), d(24), 0, nullptr, DI_NORMAL);
            RECT title{area.left + d(218), area.top + d(12), area.right - d(10), area.top + d(52)};
            DrawTextW(dc, view.source.name.c_str(), -1, &title, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            wchar_t identity[160]{};
            swprintf_s(identity, L"%ls: 0x%llX   PID: %lu", view.source.kind == SourceKind::Window ? L"HWND" : L"Monitor",
                static_cast<unsigned long long>(view.source.handle), static_cast<unsigned long>(view.source.processId));
            RECT subtitle{area.left + d(182), area.top + d(65), area.right - d(10), area.top + d(92)};
            DrawTextW(dc, identity, -1, &subtitle, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        RestoreDC(dc, -1);
        SelectObject(dc, oldFont);
        EndPaint(window_, &paint);
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: create(); return 0;
        case WM_PAINT: paint(); return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lParam)->idFrom == SourceTab &&
                reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE) { refresh(); return 0; }
            break;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PtInRect(&sourceArea_, point)) {
                const int index = (point.y - sourceArea_.top + offset_) / d(110);
                if (index >= 0 && index < static_cast<int>(sources_.size())) {
                    SetFocus(window_);
                    selected_ = index; EnableWindow(start_, TRUE); InvalidateRect(window_, &sourceArea_, TRUE);
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if ((wParam == VK_DOWN || wParam == VK_UP || wParam == VK_HOME || wParam == VK_END) && !sources_.empty()) {
                if (wParam == VK_HOME) selected_ = 0;
                else if (wParam == VK_END) selected_ = static_cast<int>(sources_.size()) - 1;
                else selected_ = std::clamp(selected_ + (wParam == VK_DOWN ? 1 : -1), 0, static_cast<int>(sources_.size()) - 1);
                const int top = selected_ * d(110);
                const int height = sourceArea_.bottom - sourceArea_.top;
                if (top < offset_) offset_ = top;
                else if (top + d(104) > offset_ + height) offset_ = top + d(104) - height;
                EnableWindow(start_, TRUE); updateScroll(); InvalidateRect(window_, &sourceArea_, TRUE);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            offset_ -= GET_WHEEL_DELTA_WPARAM(wParam) * d(110) / WHEEL_DELTA;
            updateScroll(); InvalidateRect(window_, &sourceArea_, TRUE); return 0;
        case WM_VSCROLL: {
            SCROLLINFO scroll{sizeof(scroll), SIF_TRACKPOS};
            GetScrollInfo(scrollbar_, SB_CTL, &scroll);
            switch (LOWORD(wParam)) {
            case SB_LINEUP: offset_ -= d(110); break;
            case SB_LINEDOWN: offset_ += d(110); break;
            case SB_PAGEUP: offset_ -= sourceArea_.bottom - sourceArea_.top; break;
            case SB_PAGEDOWN: offset_ += sourceArea_.bottom - sourceArea_.top; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: offset_ = scroll.nTrackPos; break;
            }
            updateScroll(); InvalidateRect(window_, &sourceArea_, TRUE); return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == CancelDialog || LOWORD(wParam) == IDCANCEL) { DestroyWindow(window_); return 0; }
            if (LOWORD(wParam) == StartTransmission || LOWORD(wParam) == IDOK) {
                if (selected_ >= 0 && selected_ < static_cast<int>(sources_.size())) {
                    ShareChoice choice; choice.source = sources_[static_cast<std::size_t>(selected_)].source;
                    if (SendMessageW(quality_, CB_GETCURSEL, 0, 0) == 1) {
                        choice.quality.width = 1920; choice.quality.height = 1080;
                    }
                    choice.quality.fps = SendMessageW(fps_, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
                    choice.quality.bitrate = choice.quality.height == 1080 ? 5000000 : 2500000;
                    if (choice.quality.fps == 60) choice.quality.bitrate = choice.quality.bitrate * 3 / 2;
                    choice.audio = SendMessageW(audio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    result_ = std::move(choice);
                    DestroyWindow(window_);
                }
                return 0;
            }
            break;
        case WM_CLOSE: DestroyWindow(window_); return 0;
        case WM_DESTROY: releaseSources(); return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
};

class DevicePicker {
public:
    DevicePicker(HINSTANCE instance, HWND owner, HFONT font, UINT dpi,
                 std::wstring input, std::wstring output)
        : instance_(instance), owner_(owner), font_(font), dpi_(dpi),
          inputId_(std::move(input)), outputId_(std::move(output)) {}
    std::optional<std::pair<std::wstring, std::wstring>> show() {
        inputs_ = AudioEngine::enumerateDevices(true);
        outputs_ = AudioEngine::enumerateDevices(false);
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = procedure; cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        cls.lpszClassName = DeviceClass; RegisterClassExW(&cls);
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, DeviceClass, L"Dispositivos de áudio",
            WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, d(620), d(265),
            owner_, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("Não foi possível abrir a seleção de dispositivos.");
        centerOverOwner(window_, owner_); modalLoop(window_, owner_);
        return result_;
    }
private:
    HINSTANCE instance_{}; HWND owner_{}, window_{}, input_{}, output_{}; HFONT font_{}; UINT dpi_{};
    std::wstring inputId_, outputId_;
    std::vector<AudioDevice> inputs_, outputs_;
    std::optional<std::pair<std::wstring, std::wstring>> result_;
    int d(int value) const { return scaled(value, dpi_); }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<DevicePicker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<DevicePicker*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->handle(message, wParam, lParam); }
        catch (const std::exception& error) { messageError(window, error.what()); return message == WM_CREATE ? -1 : 0; }
    }
    void fill(HWND combo, const std::vector<AudioDevice>& devices, const std::wstring& selected) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Padrão de comunicações do Windows"));
        int index = 0;
        for (std::size_t item = 0; item < devices.size(); ++item) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(devices[item].name.c_str()));
            if (devices[item].id == selected) index = static_cast<int>(item) + 1;
        }
        SendMessageW(combo, CB_SETCURSEL, index, 0);
    }
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_CREATE) {
            auto label1 = control(window_, instance_, font_, L"STATIC", L"Microfone", SS_LEFT, 0);
            auto label2 = control(window_, instance_, font_, L"STATIC", L"Saída da chamada", SS_LEFT, 0);
            input_ = control(window_, instance_, font_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, InputDevice);
            output_ = control(window_, instance_, font_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, OutputDevice);
            auto okay = control(window_, instance_, font_, L"BUTTON", L"Aplicar", BS_DEFPUSHBUTTON | WS_TABSTOP, ApplyDevices);
            auto cancel = control(window_, instance_, font_, L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP, CancelDialog);
            MoveWindow(label1, d(16), d(18), d(155), d(25), TRUE);
            MoveWindow(input_, d(175), d(14), d(400), d(180), TRUE);
            MoveWindow(label2, d(16), d(65), d(155), d(25), TRUE);
            MoveWindow(output_, d(175), d(61), d(400), d(180), TRUE);
            MoveWindow(okay, d(350), d(150), d(105), d(32), TRUE);
            MoveWindow(cancel, d(470), d(150), d(105), d(32), TRUE);
            fill(input_, inputs_, inputId_); fill(output_, outputs_, outputId_);
            return 0;
        }
        if (message == WM_COMMAND) {
            if (LOWORD(wParam) == ApplyDevices || LOWORD(wParam) == IDOK) {
                const auto a = SendMessageW(input_, CB_GETCURSEL, 0, 0);
                const auto b = SendMessageW(output_, CB_GETCURSEL, 0, 0);
                result_ = std::make_pair(a > 0 ? inputs_.at(static_cast<std::size_t>(a - 1)).id : L"",
                                         b > 0 ? outputs_.at(static_cast<std::size_t>(b - 1)).id : L"");
                DestroyWindow(window_); return 0;
            }
            if (LOWORD(wParam) == CancelDialog || LOWORD(wParam) == IDCANCEL) { DestroyWindow(window_); return 0; }
        }
        if (message == WM_CLOSE) { DestroyWindow(window_); return 0; }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
};

struct Mailbox {
    std::mutex mutex;
    HWND window{};
    bool closed{};
    std::deque<std::string> errors;
    void changed() {
        std::lock_guard lock(mutex);
        if (!closed && window) PostMessageW(window, UpdateMessage, 0, 0);
    }
    void error(std::string value) {
        std::lock_guard lock(mutex);
        if (closed) return;
        // Do not let a failing device or network flood the UI indefinitely.
        if (errors.size() < 8) errors.push_back(std::move(value));
        if (window) PostMessageW(window, ErrorMessage, 0, 0);
    }
};

class MainWindow {
public:
    MainWindow(HINSTANCE instance, ConnectionSettings settings)
        : instance_(instance), settings_(std::move(settings)), mailbox_(std::make_shared<Mailbox>()) {}
    ~MainWindow() {
        {
            std::lock_guard lock(mailbox_->mutex);
            mailbox_->closed = true; mailbox_->window = nullptr;
        }
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        controller_.reset();
        if (font_) DeleteObject(font_);
    }

    int run(int showCommand) {
        registerClasses();
        dpi_ = GetDpiForSystem();
        font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0,
                           FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!font_) throw std::runtime_error("Não foi possível criar a fonte da interface.");
        window_ = CreateWindowExW(0, MainClass, L"Native Call — Voz e compartilhamento de tela",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, d(1180), d(860),
            nullptr, nullptr, instance_, this);
        if (!window_) throw std::runtime_error("Não foi possível criar a janela principal.");
        {
            std::lock_guard lock(mailbox_->mutex); mailbox_->window = window_;
        }
        auto mailbox = mailbox_;
        controller_ = std::make_unique<CallController>(CallController::Callbacks{
            [mailbox] { mailbox->changed(); },
            [mailbox](std::string error) { mailbox->error(std::move(error)); }});
        update();
        ShowWindow(window_, showCommand); UpdateWindow(window_);
        MSG message{};
        for (;;) {
            const auto result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) return result ? 1 : static_cast<int>(message.wParam);
            if (message.message == WM_KEYDOWN && message.wParam == VK_F11) { toggleFullscreen(); continue; }
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && fullscreen_) { toggleFullscreen(); continue; }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
    }

private:
    struct Row {
        HWND name{}, voiceLabel{}, voice{}, shareLabel{}, shared{}, watch{}, mute{};
        std::string peer;
    };
    HINSTANCE instance_{};
    HWND window_{}, video_{};
    UINT dpi_{96};
    HFONT font_{};
    ConnectionSettings settings_;
    std::shared_ptr<Mailbox> mailbox_;
    std::unique_ptr<CallController> controller_;
    AppSnapshot snapshot_;
    std::map<int, HWND> controls_;
    std::vector<HWND> labels_;
    std::array<Row, MaxRows> rows_{};
    std::map<std::string, float> savedShareVolume_;
    std::string watching_;
    std::wstring inputDevice_, outputDevice_;
    bool fullscreen_{};
    WINDOWPLACEMENT savedPlacement_{sizeof(savedPlacement_)};

    int d(int value) const { return scaled(value, dpi_); }

    void registerClasses() {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = procedure; cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); cls.lpszClassName = MainClass;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Não foi possível registrar a janela principal.");
        cls.lpfnWndProc = videoProcedure; cls.lpszClassName = VideoClass;
        cls.style = CS_DBLCLKS; cls.hbrBackground = nullptr;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Não foi possível registrar o visualizador de vídeo.");
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->handle(message, wParam, lParam); }
        catch (const std::exception& exception) { messageError(window, exception.what()); return message == WM_CREATE ? -1 : 0; }
    }

    static LRESULT CALLBACK videoProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT && self) { self->paintVideo(window); return 0; }
        if (message == WM_LBUTTONDBLCLK && self) { PostMessageW(self->window_, FullscreenMessage, 0, 0); return 0; }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD extendedStyle = 0) {
        const HWND result = control(window_, instance_, font_, cls, text, style, id, extendedStyle);
        if (id) controls_[id] = result;
        else labels_.push_back(result);
        return result;
    }

    void create() {
        add(L"STATIC", L"Sinalização", SS_LEFT, 0);
        add(L"EDIT", wide(settings_.signalingUrl).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, Server, WS_EX_CLIENTEDGE);
        add(L"STATIC", L"Sala", SS_LEFT, 0);
        add(L"EDIT", wide(settings_.room).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, Room, WS_EX_CLIENTEDGE);
        add(L"STATIC", L"Nome", SS_LEFT, 0);
        add(L"EDIT", wide(settings_.displayName).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, Name, WS_EX_CLIENTEDGE);
        add(L"STATIC", L"Token", SS_LEFT, 0);
        add(L"EDIT", wide(settings_.accessToken).c_str(), ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, Token, WS_EX_CLIENTEDGE);
        SendMessageW(controls_[Server], EM_SETLIMITTEXT, 2048, 0);
        SendMessageW(controls_[Room], EM_SETLIMITTEXT, 64, 0);
        SendMessageW(controls_[Name], EM_SETLIMITTEXT, 80, 0);
        SendMessageW(controls_[Token], EM_SETLIMITTEXT, 2048, 0);
        add(L"BUTTON", L"Entrar", BS_PUSHBUTTON | WS_TABSTOP, Join);
        add(L"BUTTON", L"Sair", BS_PUSHBUTTON | WS_TABSTOP, Leave);
        add(L"BUTTON", L"Dispositivos de áudio", BS_PUSHBUTTON | WS_TABSTOP, Devices);
        add(L"BUTTON", L"Silenciar microfone", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, Microphone);
        add(L"BUTTON", L"Silenciar recebidos", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, Deafen);
        add(L"BUTTON", L"Compartilhar tela", BS_PUSHBUTTON | WS_TABSTOP, Share);
        add(L"BUTTON", L"Parar transmissão", BS_PUSHBUTTON | WS_TABSTOP, StopShare);
        add(L"BUTTON", L"Silenciar áudio enviado", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, MuteShare);
        add(L"BUTTON", L"Tela cheia (F11)", BS_PUSHBUTTON | WS_TABSTOP, Fullscreen);
        add(L"STATIC", L"Desconectado", SS_LEFT, Status);
        for (int index = 0; index < MaxRows; ++index) {
            auto& row = rows_[static_cast<std::size_t>(index)];
            const int id = RowBase + index * 10;
            row.name = add(L"STATIC", L"", SS_LEFT, id);
            row.voiceLabel = add(L"STATIC", L"Voz: 100%", SS_LEFT, id + 1);
            row.voice = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, id + 2);
            row.shareLabel = add(L"STATIC", L"Transmissão: 100%", SS_LEFT, id + 3);
            row.shared = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, id + 4);
            row.watch = add(L"BUTTON", L"Assistir", BS_PUSHBUTTON | WS_TABSTOP, id + 5);
            row.mute = add(L"BUTTON", L"Silenciar transmissão", BS_AUTOCHECKBOX | WS_TABSTOP, id + 6);
            for (auto slider : {row.voice, row.shared}) {
                SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
                SendMessageW(slider, TBM_SETTICFREQ, 50, 0);
                SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
                SendMessageW(slider, TBM_SETPOS, TRUE, 100);
            }
        }
        video_ = CreateWindowExW(WS_EX_CLIENTEDGE, VideoClass, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 1, 1, window_, nullptr, instance_, this);
        if (!video_) throw std::runtime_error("Não foi possível criar o visualizador.");
        layout();
        SetTimer(window_, 1, 33, nullptr);
    }

    void layout() {
        if (!video_) return;
        RECT client{}; GetClientRect(window_, &client);
        if (fullscreen_) { MoveWindow(video_, 0, 0, client.right, client.bottom, TRUE); return; }
        const auto move = [this](int id, int x, int y, int width, int height) {
            MoveWindow(controls_.at(id), d(x), d(y), d(width), d(height), TRUE);
        };
        MoveWindow(labels_[0], d(12), d(14), d(88), d(24), TRUE);
        move(Server, 105, 9, 570, 27);
        MoveWindow(labels_[1], d(700), d(14), d(40), d(24), TRUE);
        move(Room, 745, 9, std::max(160, static_cast<int>(client.right) * 96 / static_cast<int>(dpi_) - 760), 27);
        MoveWindow(labels_[2], d(12), d(52), d(88), d(24), TRUE);
        move(Name, 105, 46, 245, 27);
        MoveWindow(labels_[3], d(365), d(52), d(55), d(24), TRUE);
        move(Token, 425, 46, 250, 27);
        move(Join, 695, 45, 80, 29); move(Leave, 785, 45, 70, 29);
        move(Devices, 865, 45, 245, 29);
        move(Microphone, 12, 92, 165, 32); move(Deafen, 187, 92, 170, 32);
        move(Share, 367, 92, 155, 32); move(StopShare, 532, 92, 150, 32);
        move(MuteShare, 692, 92, 200, 32); move(Fullscreen, 902, 92, 180, 32);
        MoveWindow(controls_[Status], d(12), d(137), client.right - d(24), d(42), TRUE);
        for (int index = 0; index < MaxRows; ++index) {
            auto& row = rows_[static_cast<std::size_t>(index)];
            const int y = 185 + index * 58;
            MoveWindow(row.name, d(14), d(y + 5), d(230), d(46), TRUE);
            MoveWindow(row.voiceLabel, d(250), d(y), d(150), d(20), TRUE);
            MoveWindow(row.voice, d(245), d(y + 18), d(185), d(30), TRUE);
            MoveWindow(row.shareLabel, d(448), d(y), d(170), d(20), TRUE);
            MoveWindow(row.shared, d(443), d(y + 18), d(185), d(30), TRUE);
            MoveWindow(row.watch, d(650), d(y + 9), d(95), d(30), TRUE);
            MoveWindow(row.mute, d(770), d(y + 11), d(245), d(28), TRUE);
        }
        MoveWindow(video_, d(12), d(425), client.right - d(24),
                   std::max(d(90), static_cast<int>(client.bottom) - d(437)), TRUE);
    }

    void update() {
        if (!controller_) return;
        snapshot_ = controller_->snapshot();
        auto status = wide(snapshot_.connectionState);
        if (snapshot_.sharing) {
            status += L"  •  AO VIVO: " + snapshot_.sourceName + L"  (" +
                std::to_wstring(snapshot_.quality.height) + L"p / " + std::to_wstring(snapshot_.quality.fps) + L" FPS)";
            if (snapshot_.shareMuted) status += L" — áudio compartilhado silenciado";
        }
        setText(controls_[Status], status);
        EnableWindow(controls_[Join], !snapshot_.joined && !snapshot_.joining);
        EnableWindow(controls_[Leave], snapshot_.joined || snapshot_.joining);
        EnableWindow(controls_[Devices], !snapshot_.sharing && !snapshot_.joining);
        for (int id : {Microphone, Deafen, Share}) EnableWindow(controls_[id], snapshot_.joined);
        for (int id : {StopShare, MuteShare}) EnableWindow(controls_[id], snapshot_.sharing);
        for (int id : {Server, Room, Name, Token})
            EnableWindow(controls_[id], !snapshot_.joined && !snapshot_.joining);
        SendMessageW(controls_[Microphone], BM_SETCHECK, snapshot_.micMuted ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(controls_[Deafen], BM_SETCHECK, snapshot_.deafened ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(controls_[MuteShare], BM_SETCHECK, snapshot_.shareMuted ? BST_CHECKED : BST_UNCHECKED, 0);
        setText(controls_[Share], snapshot_.sharing ? L"Trocar fonte" : L"Compartilhar tela");
        bool watchingExists = false;
        for (const auto& participant : snapshot_.participants)
            if (participant.id != "self" && participant.id == watching_ && participant.sharing) watchingExists = true;
        if (!watchingExists) {
            watching_.clear();
            for (const auto& participant : snapshot_.participants)
                if (participant.id != "self" && participant.sharing) { watching_ = participant.id; break; }
        }
        for (int index = 0; index < MaxRows; ++index) {
            auto& row = rows_[static_cast<std::size_t>(index)];
            const bool exists = index < static_cast<int>(snapshot_.participants.size());
            for (HWND child : {row.name, row.voiceLabel, row.voice, row.shareLabel, row.shared, row.watch, row.mute})
                ShowWindow(child, exists && !fullscreen_ ? SW_SHOW : SW_HIDE);
            if (!exists) { row.peer.clear(); continue; }
            const auto& participant = snapshot_.participants[static_cast<std::size_t>(index)];
            row.peer = participant.id;
            std::wstring caption = participant.speaking ? L"● Falando: " : L"○ ";
            caption += wide(participant.name);
            if (participant.sharing) caption += L"\r\nAo vivo: " + wide(participant.sharedSource);
            setText(row.name, caption);
            const auto voice = std::clamp(static_cast<int>(std::lround(participant.voiceVolume * 100)), 0, 200);
            const auto shared = std::clamp(static_cast<int>(std::lround(participant.shareVolume * 100)), 0, 200);
            if (GetCapture() != row.voice) SendMessageW(row.voice, TBM_SETPOS, TRUE, voice);
            if (GetCapture() != row.shared) SendMessageW(row.shared, TBM_SETPOS, TRUE, shared);
            setText(row.voiceLabel, L"Voz: " + std::to_wstring(voice) + L"%");
            setText(row.shareLabel, L"Transmissão: " + std::to_wstring(shared) + L"%");
            SendMessageW(row.mute, BM_SETCHECK, shared == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
            const bool remote = participant.id != "self";
            EnableWindow(row.voice, remote);
            EnableWindow(row.shared, remote);
            EnableWindow(row.mute, remote);
            EnableWindow(row.watch, remote && participant.sharing);
            setText(row.watch, watching_ == participant.id ? L"Assistindo" : L"Assistir");
        }
        InvalidateRect(video_, nullptr, FALSE);
    }

    void command(int id) {
        if (!controller_) return;
        if (id >= RowBase && id < RowBase + MaxRows * 10) {
            auto& row = rows_.at(static_cast<std::size_t>((id - RowBase) / 10));
            if (row.peer.empty()) return;
            const int action = (id - RowBase) % 10;
            if (action == 5) { watching_ = row.peer; update(); }
            if (action == 6) {
                const bool mute = SendMessageW(row.mute, BM_GETCHECK, 0, 0) == BST_CHECKED;
                float gain = 1.0f;
                for (const auto& peer : snapshot_.participants) if (peer.id == row.peer) gain = peer.shareVolume;
                if (mute) {
                    savedShareVolume_[row.peer] = gain > 0 ? gain : 1.0f;
                    controller_->setVolume(row.peer, AudioKind::Shared, 0);
                } else {
                    const auto found = savedShareVolume_.find(row.peer);
                    controller_->setVolume(row.peer, AudioKind::Shared, found == savedShareVolume_.end() ? 1.0f : found->second);
                }
            }
            return;
        }
        switch (id) {
        case Join:
            settings_.signalingUrl = utf8(windowText(controls_[Server]));
            settings_.room = utf8(windowText(controls_[Room]));
            settings_.displayName = utf8(windowText(controls_[Name]));
            settings_.accessToken = utf8(windowText(controls_[Token]));
            if (settings_.room.empty() || settings_.displayName.empty() || settings_.signalingUrl.empty())
                throw std::runtime_error("Preencha o servidor de sinalização, a sala e seu nome.");
            controller_->join(settings_); break;
        case Leave: controller_->leave(); break;
        case Microphone: controller_->setMicrophoneMuted(SendMessageW(controls_[Microphone], BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case Deafen: controller_->setDeafened(SendMessageW(controls_[Deafen], BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case Share: {
            SharePicker picker(instance_, window_, font_, dpi_);
            if (const auto choice = picker.show()) controller_->startSharing(choice->source, choice->quality, choice->audio);
            break;
        }
        case StopShare: controller_->stopSharing(); break;
        case MuteShare: controller_->setShareMuted(SendMessageW(controls_[MuteShare], BM_GETCHECK, 0, 0) == BST_CHECKED); break;
        case Devices: {
            DevicePicker picker(instance_, window_, font_, dpi_, inputDevice_, outputDevice_);
            if (const auto choice = picker.show()) {
                inputDevice_ = choice->first; outputDevice_ = choice->second;
                controller_->selectDevices(inputDevice_, outputDevice_);
            }
            break;
        }
        case Fullscreen: toggleFullscreen(); break;
        }
    }

    void toggleFullscreen() {
        if (!window_) return;
        if (!fullscreen_) {
            savedPlacement_.length = sizeof(savedPlacement_);
            GetWindowPlacement(window_, &savedPlacement_);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowLongPtrW(window_, GWL_STYLE, GetWindowLongPtrW(window_, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
            fullscreen_ = true;
            SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        } else {
            fullscreen_ = false;
            SetWindowLongPtrW(window_, GWL_STYLE, GetWindowLongPtrW(window_, GWL_STYLE) | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(window_, &savedPlacement_);
            SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        for (const auto& [id, child] : controls_) ShowWindow(child, fullscreen_ ? SW_HIDE : SW_SHOW);
        for (HWND child : labels_) ShowWindow(child, fullscreen_ ? SW_HIDE : SW_SHOW);
        layout(); update();
        SetFocus(video_);
    }

    void paintVideo(HWND video) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(video, &paint);
        RECT client{}; GetClientRect(video, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        std::shared_ptr<const VideoFrame> frame;
        std::wstring title;
        for (const auto& participant : snapshot_.participants) if (participant.id == watching_) {
            frame = participant.video;
            title = wide(participant.name) + L" — " + wide(participant.sharedSource);
            break;
        }
        if (frame && frame->width && frame->height && frame->stride >= frame->width * 4ULL &&
            frame->bgra.size() >= static_cast<std::size_t>(frame->stride) * frame->height &&
            frame->width <= 16384 && frame->height <= 16384) {
            const double ratio = std::min(static_cast<double>(client.right) / frame->width,
                                          static_cast<double>(client.bottom) / frame->height);
            const int width = static_cast<int>(frame->width * ratio);
            const int height = static_cast<int>(frame->height * ratio);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(frame->stride / 4);
            info.bmiHeader.biHeight = -static_cast<LONG>(frame->height);
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, (client.right - width) / 2, (client.bottom - height) / 2, width, height,
                0, 0, static_cast<int>(frame->width), static_cast<int>(frame->height), frame->bgra.data(),
                &info, DIB_RGB_COLORS, SRCCOPY);
        } else {
            auto oldFont = SelectObject(dc, font_);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(220, 220, 220));
            RECT text = client;
            DrawTextW(dc, watching_.empty() ? L"As transmissões dos participantes aparecerão aqui."
                                           : L"Aguardando o vídeo da transmissão…", -1, &text,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, oldFont);
        }
        if (!title.empty() || fullscreen_) {
            auto oldFont = SelectObject(dc, font_);
            SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, RGB(25, 25, 25)); SetBkMode(dc, OPAQUE);
            RECT caption{d(12), d(8), client.right - d(12), d(36)};
            if (fullscreen_) title += L"  •  Esc: sair da tela cheia";
            DrawTextW(dc, title.c_str(), -1, &caption, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, oldFont);
        }
        EndPaint(video, &paint);
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: create(); return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {d(1150), d(750)}; return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const HFONT replacement = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0,
                FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (replacement) {
                EnumChildWindows(window_, [](HWND child, LPARAM parameter) -> BOOL {
                    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
                    return TRUE;
                }, reinterpret_cast<LPARAM>(replacement));
                DeleteObject(font_); font_ = replacement;
            }
            const auto* rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(); return 0;
        }
        case WM_COMMAND: if (HIWORD(wParam) == BN_CLICKED) command(LOWORD(wParam)); return 0;
        case WM_HSCROLL:
            if (controller_) for (auto& row : rows_) {
                const HWND slider = reinterpret_cast<HWND>(lParam);
                if (!row.peer.empty() && (slider == row.voice || slider == row.shared)) {
                    const float gain = static_cast<float>(SendMessageW(slider, TBM_GETPOS, 0, 0)) / 100;
                    controller_->setVolume(row.peer, slider == row.voice ? AudioKind::Microphone : AudioKind::Shared, gain);
                }
            }
            return 0;
        case UpdateMessage: update(); return 0;
        case ErrorMessage: {
            std::deque<std::string> errors;
            { std::lock_guard lock(mailbox_->mutex); errors.swap(mailbox_->errors); }
            if (!errors.empty()) {
                std::string text;
                for (const auto& error : errors) { if (!text.empty()) text += "\n\n"; text += error; }
                messageError(window_, text);
            }
            return 0;
        }
        case FullscreenMessage: toggleFullscreen(); return 0;
        case WM_TIMER: if (wParam == 1) update(); return 0;
        case WM_CLOSE: DestroyWindow(window_); return 0;
        case WM_DESTROY:
            KillTimer(window_, 1);
            { std::lock_guard lock(mailbox_->mutex); mailbox_->closed = true; mailbox_->window = nullptr; }
            controller_.reset();
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
};

} // namespace

int runNativeApplication(HINSTANCE instance, int showCommand, ConnectionSettings settings) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Não foi possível iniciar os controles do Windows.");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) throw std::runtime_error("Não foi possível iniciar o COM na interface.");
    try {
        int result;
        {
            MainWindow window(instance, std::move(settings));
            result = window.run(showCommand);
        }
        CoUninitialize();
        return result;
    } catch (...) {
        CoUninitialize();
        throw;
    }
}

} // namespace nativecall
