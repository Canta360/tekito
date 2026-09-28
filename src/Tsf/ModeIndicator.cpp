#include "Tsf/ModeIndicator.h"

#include "Tsf/ComPtr.h"
#include "assets/tekito_resource.h"

#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <vector>

namespace tekito::tsf {
namespace {

constexpr wchar_t kWindowClass[] = L"TekitoModeIndicator";
constexpr UINT_PTR kFadeTimer = 1;
// Shown in full, then faded out over kFadeSteps ticks.
constexpr UINT kHoldMs = 850;
constexpr UINT kFadeTickMs = 25;
constexpr int kFadeSteps = 8;
// In DIPs.
constexpr float kTile = 40.0f;
constexpr float kIcon = 24.0f;
constexpr float kRadius = 9.0f;
constexpr float kGap = 6.0f;

// The apps' theme, as the candidate list follows it.
bool AppsAreDark() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    wchar_t theme[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_THEME", theme, 16) > 0) return wcscmp(theme, L"dark") == 0;
#endif
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

int ModeIcon(InputMode mode, bool dark) {
    switch (mode) {
    case InputMode::Japanese: return dark ? IDI_JAPANESE_DARK : IDI_JAPANESE;
    case InputMode::Direct: return dark ? IDI_DIRECT_DARK : IDI_DIRECT;
    default: return IDI_AUTO;
    }
}

// The icon's pixels, premultiplied BGRA, top-down.
bool IconPixels(HICON icon, int side, std::vector<std::uint32_t>& pixels) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return false;
    bool ok = false;
    if (info.hbmColor) {
        BITMAPINFO bitmap{};
        bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
        pixels.assign(static_cast<std::size_t>(side) * side, 0);
        HDC screen = GetDC(nullptr);
        ok = GetDIBits(screen, info.hbmColor, 0, static_cast<UINT>(side), pixels.data(), &bitmap,
                       DIB_RGB_COLORS) == side;
        ReleaseDC(nullptr, screen);
        for (auto& pixel : pixels) {
            const std::uint32_t alpha = pixel >> 24;
            const auto channel = [&](int shift) { return ((pixel >> shift) & 0xFF) * alpha / 255; };
            pixel = (alpha << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
        }
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return ok;
}

ID2D1Factory* Factory() {
    static ID2D1Factory* factory = [] {
        ID2D1Factory* created = nullptr;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &created);
        return created;
    }();
    return factory;
}

}  // namespace

ModeIndicator::~ModeIndicator() { Shutdown(); }

void ModeIndicator::Shutdown() noexcept {
    if (hwnd_) {
        KillTimer(hwnd_, kFadeTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ReleaseFrame();
}

void ModeIndicator::ReleaseFrame() noexcept {
    if (frameDc_) {
        if (previousBitmap_) SelectObject(frameDc_, previousBitmap_);
        DeleteDC(frameDc_);
    }
    if (frame_) DeleteObject(frame_);
    frameDc_ = nullptr;
    frame_ = nullptr;
    previousBitmap_ = nullptr;
}

bool ModeIndicator::EnsureWindow(HINSTANCE instance) {
    if (hwnd_) return true;
    // A class left registered by an earlier load of this DLL points at code
    // that is gone: register it afresh once per load.
    static const ATOM atom = [instance] {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &ModeIndicator::WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClass;
        ATOM registered = RegisterClassExW(&windowClass);
        if (!registered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS &&
            UnregisterClassW(kWindowClass, instance)) {
            registered = RegisterClassExW(&windowClass);
        }
        return registered;
    }();
    if (!atom) return false;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE,
                            kWindowClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

LRESULT CALLBACK ModeIndicator::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ModeIndicator*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_TIMER && wParam == kFadeTimer && self) {
        // The first tick ends the hold; the rest fade.
        if (self->fadeStep_ == 0) SetTimer(hwnd, kFadeTimer, kFadeTickMs, nullptr);
        ++self->fadeStep_;
        if (self->fadeStep_ > kFadeSteps) {
            self->Hide();
        } else {
            self->Present(static_cast<BYTE>(255 * (kFadeSteps - self->fadeStep_) / kFadeSteps));
        }
        return 0;
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ModeIndicator::Show(HINSTANCE instance, const RECT& caret, InputMode mode) {
    if (!EnsureWindow(instance)) return;
    // Place the window on the caret's monitor first, so its DPI is that one.
    HMONITOR monitor = MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST);
    MONITORINFO area{sizeof(area)};
    GetMonitorInfoW(monitor, &area);
    SetWindowPos(hwnd_, HWND_TOPMOST, caret.left, caret.bottom, 1, 1, SWP_NOACTIVATE | SWP_NOREDRAW);
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(hwnd_));
    if (!Render(instance, mode, dpi)) return;

    // Under the caret, or above it at the bottom of the screen.
    const int gap = static_cast<int>(std::lround(kGap * dpi / 96.0f));
    const RECT work = area.rcWork;
    POINT position{caret.left, caret.bottom + gap};
    if (position.y + size_.cy > work.bottom) position.y = caret.top - gap - size_.cy;
    position.x = std::clamp<LONG>(position.x, work.left, std::max(work.left, work.right - size_.cx));
    position.y = std::clamp<LONG>(position.y, work.top, std::max(work.top, work.bottom - size_.cy));
    position_ = position;

    fadeStep_ = 0;
    Present(255);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTimer(hwnd_, kFadeTimer, kHoldMs, nullptr);
}

void ModeIndicator::Hide() noexcept {
    if (!hwnd_) return;
    KillTimer(hwnd_, kFadeTimer);
    ShowWindow(hwnd_, SW_HIDE);
}

bool ModeIndicator::Render(HINSTANCE instance, InputMode mode, UINT dpi) {
    ID2D1Factory* factory = Factory();
    if (!factory) return false;
    const float scale = static_cast<float>(dpi) / 96.0f;
    const int side = static_cast<int>(std::lround(kTile * scale));
    const int iconSide = static_cast<int>(std::lround(kIcon * scale));

    ReleaseFrame();
    BITMAPINFO bitmap{};
    bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), side, -side, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    frameDc_ = CreateCompatibleDC(screen);
    frame_ = CreateDIBSection(screen, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!frameDc_ || !frame_) {
        ReleaseFrame();
        return false;
    }
    previousBitmap_ = SelectObject(frameDc_, frame_);
    size_ = {side, side};

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1DCRenderTarget> target;
    if (FAILED(factory->CreateDCRenderTarget(&properties, target.Put()))) return false;
    const RECT bounds{0, 0, side, side};
    if (FAILED(target->BindDC(frameDc_, &bounds))) return false;

    const bool dark = AppsAreDark();
    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> edge;
    target->CreateSolidColorBrush(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.96f)
                                       : D2D1::ColorF(0.99f, 0.99f, 0.99f, 0.97f),
                                  fill.Put());
    target->CreateSolidColorBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
                                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f),
                                  edge.Put());
    if (!fill || !edge) return false;

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    const float half = 0.5f;
    const D2D1_ROUNDED_RECT tile{D2D1::RectF(half, half, side - half, side - half), kRadius * scale,
                                 kRadius * scale};
    target->FillRoundedRectangle(tile, fill.Get());
    target->DrawRoundedRectangle(tile, edge.Get(), 1.0f);

    HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(ModeIcon(mode, dark)), IMAGE_ICON,
                                               iconSide, iconSide, LR_DEFAULTCOLOR));
    std::vector<std::uint32_t> pixels;
    if (icon && IconPixels(icon, iconSide, pixels)) {
        ComPtr<ID2D1Bitmap> image;
        const D2D1_BITMAP_PROPERTIES imageProperties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(target->CreateBitmap(D2D1::SizeU(iconSide, iconSide), pixels.data(),
                                           static_cast<UINT32>(iconSide * 4), &imageProperties, image.Put()))) {
            const float offset = (side - iconSide) / 2.0f;
            target->DrawBitmap(image.Get(), D2D1::RectF(offset, offset, offset + iconSide, offset + iconSide));
        }
    }
    if (icon) DestroyIcon(icon);
    return SUCCEEDED(target->EndDraw());
}

void ModeIndicator::Present(BYTE opacity) noexcept {
    if (!hwnd_ || !frameDc_) return;
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, &position_, &size_, frameDc_, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

}  // namespace tekito::tsf
