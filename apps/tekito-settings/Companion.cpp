#include "apps/tekito-settings/Companion.h"

#include "apps/tekito-settings/CompanionShapes.h"
#include "UserData/RuntimeModeState.h"
#include "UserData/UiLanguage.h"
#include "UserData/UserDataRepository.h"
#include "UserData/UserSettings.h"

#include <d2d1_1.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"TekitoCompanion";
constexpr wchar_t kSingleInstance[] = L"Local\\TEKITO.Companion";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TEKITO Companion";
// Where it was left: its window's top left, in screen pixels.
constexpr wchar_t kPlaceKey[] = L"Software\\TEKITO\\Companion";

// Its size and distance from the screen's corner, in DIPs; the window is a
// little taller than the body, for its hop.
constexpr float kSize = 88.0f;
constexpr float kHeadroom = 0.08f;
constexpr float kCornerGap = 24.0f;
// Pixels the pointer moves before a press becomes a drag.
constexpr int kDragSlop = 4;
constexpr UINT kFrameMilliseconds = 33;
constexpr UINT_PTR kFrameTimer = 1;
// Settings, secret mode and full-screen apps are looked at every second.
constexpr UINT_PTR kCheckTimer = 2;
constexpr UINT kCheckMilliseconds = 1000;

// As on Home (mascot.jsx, styles.css), in the drawing's units and seconds.
constexpr float kLinePen = 30.0f;
constexpr double kFloatPeriod = 3.2;
constexpr float kFloatHeight = 14.0f;
constexpr double kBlinkPeriod = 4.6;
constexpr double kHopTime = 0.5;
constexpr int kHops = 2;
constexpr float kHopHeight = 44.0f;
constexpr double kHappyTime = 1.4;
constexpr double kRainbowPeriod = 4.0;
constexpr float kLookReach = 20.0f;
constexpr float kListeningEyes = 1.08f;
// The body's foot, which it hops and squashes from.
constexpr float kFootX = 450.0f;
constexpr float kFootY = 768.0f;

constexpr UINT kMenuSettings = 1;
constexpr UINT kMenuHide = 2;

std::wstring AppPath() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

bool AnimationsEnabled() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled != FALSE;
}

// Full-screen apps, presentations and quiet hours: no companion over them.
bool ScreenBusy() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
}

D2D1_COLOR_F Color(unsigned rgb, float alpha = 1.0f) {
    return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha};
}

// The color with its hue turned by `degrees` (Rainbow), as Home does it.
D2D1_COLOR_F Turned(D2D1_COLOR_F color, float degrees) {
    const float max = std::max({color.r, color.g, color.b});
    const float min = std::min({color.r, color.g, color.b});
    const float l = (max + min) / 2;
    const float d = max - min;
    if (d == 0.0f) return color;
    const float s = d / (1 - std::fabs(2 * l - 1));
    float h = max == color.r ? std::fmod((color.g - color.b) / d, 6.0f) : max == color.g ? (color.b - color.r) / d + 2 : (color.r - color.g) / d + 4;
    h = std::fmod(h * 60 + degrees + 720.0f, 360.0f);
    const float c = (1 - std::fabs(2 * l - 1)) * s;
    const float x = c * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1));
    const float m = l - c / 2;
    float r = 0, g = 0, b = 0;
    if (h < 60) r = c, g = x;
    else if (h < 120) r = x, g = c;
    else if (h < 180) g = c, b = x;
    else if (h < 240) g = x, b = c;
    else if (h < 300) r = x, b = c;
    else r = c, b = x;
    return {r + m, g + m, b + m, color.a};
}

// An SVG path (companionShapes.js uses M L H V C Q A Z, absolute) as a
// Direct2D geometry; lines stay open, shapes are filled.
ComPtr<ID2D1PathGeometry> ReadPath(ID2D1Factory* factory, const char* path, bool line) {
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return nullptr;
    const char* at = path;
    const auto skip = [&] {
        while (*at == ' ' || *at == ',') ++at;
    };
    const auto number = [&] {
        skip();
        char* end = nullptr;
        const float value = std::strtof(at, &end);
        at = end;
        return value;
    };
    const auto begin = line ? D2D1_FIGURE_BEGIN_HOLLOW : D2D1_FIGURE_BEGIN_FILLED;
    D2D1_POINT_2F current{};
    bool open = false;
    char command = 0;
    for (skip(); *at; skip()) {
        if ((*at >= 'A' && *at <= 'Z') || (*at >= 'a' && *at <= 'z')) command = *at++;
        switch (command) {
        case 'M': {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            current = {number(), number()};
            sink->BeginFigure(current, begin);
            open = true;
            command = 'L';
            break;
        }
        case 'L':
            current = {number(), number()};
            sink->AddLine(current);
            break;
        case 'H':
            current.x = number();
            sink->AddLine(current);
            break;
        case 'V':
            current.y = number();
            sink->AddLine(current);
            break;
        case 'C': {
            const D2D1_POINT_2F first{number(), number()};
            const D2D1_POINT_2F second{number(), number()};
            current = {number(), number()};
            sink->AddBezier({first, second, current});
            break;
        }
        case 'Q': {
            const D2D1_POINT_2F control{number(), number()};
            current = {number(), number()};
            sink->AddQuadraticBezier({control, current});
            break;
        }
        case 'A': {
            const D2D1_SIZE_F radii{number(), number()};
            const float rotation = number();
            const bool large = number() != 0.0f;
            const bool sweep = number() != 0.0f;
            current = {number(), number()};
            sink->AddArc({current, radii, rotation,
                          sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                          large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
            break;
        }
        case 'Z':
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            command = 0;
            break;
        default:
            sink->Close();
            return nullptr;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    return SUCCEEDED(sink->Close()) ? geometry : nullptr;
}

ComPtr<ID2D1PathGeometry> Without(ID2D1Factory* factory, ID2D1Geometry* shape, ID2D1Geometry* hole) {
    ComPtr<ID2D1PathGeometry> result;
    ComPtr<ID2D1GeometrySink> sink;
    if (!shape || !hole || FAILED(factory->CreatePathGeometry(&result)) || FAILED(result->Open(&sink)) ||
        FAILED(shape->CombineWithGeometry(hole, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())) ||
        FAILED(sink->Close())) {
        return nullptr;
    }
    return result;
}

ComPtr<ID2D1EllipseGeometry> Circle(ID2D1Factory* factory, float x, float y, float r) {
    ComPtr<ID2D1EllipseGeometry> circle;
    factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), &circle);
    return circle;
}

float Ease(float t) { return t * t * (3 - 2 * t); }

class Companion {
public:
    bool Create(HINSTANCE instance) {
        instance_ = instance;
        repository_ = tekito::userdata::CreateDefaultUserDataRepository();
        if (!repository_ || !repository_->Open() || !repository_->LoadSettings(settings_)) return false;
#ifndef NDEBUG
        // Debug builds: TEKITO_COMPANION_PREVIEW=1 shows it with the setting
        // off, to look at it without changing the user's settings.
        preview_ = GetEnvironmentVariableW(L"TEKITO_COMPANION_PREVIEW", nullptr, 0) > 0;
#endif
        if (!settings_.desktopCompanion && !preview_) return false;
        // Seeding a new session as Settings does (it may start first).
        auto startMode = tekito::userdata::StartMode(settings_, settings_.lastInputMode);
        if (startMode != tekito::InputMode::Direct) startMode = tekito::InputMode::Convert;
        runtime_ = std::make_unique<tekito::userdata::RuntimeModeState>(
            startMode, tekito::userdata::kRuntimeStateName,
            tekito::userdata::StartMode(settings_, settings_.lastJapaneseProfileMode));
        modeGeneration_ = runtime_->ModeGeneration();
        settingsGeneration_ = runtime_->SettingsGeneration();
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())) || !BuildShapes()) {
            return false;
        }

        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = &Companion::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
        windowClass.lpszClassName = kWindowClass;
        RegisterClassExW(&windowClass);
        const POINT at = SavedPlace();
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kWindowClass,
                                L"TEKITO", WS_POPUP, at.x, at.y, 1, 1, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        Resize(GetDpiForWindow(hwnd_));
        if (!KeepOnScreen(saved_)) MoveTo(DefaultPlace());
        started_ = std::chrono::steady_clock::now();
        motion_ = AnimationsEnabled();
        Check();
        SetTimer(hwnd_, kCheckTimer, kCheckMilliseconds, nullptr);
        return true;
    }

    int Run() {
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        }
        auto* self = reinterpret_cast<Companion*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->Handle(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_TIMER:
            if (wParam == kFrameTimer) Draw();
            if (wParam == kCheckTimer) Check();
            return 0;
        case WM_LBUTTONDOWN:
            GetCursorPos(&pressedAt_);
            pressedPlace_ = place_;
            pressed_ = true;
            dragging_ = false;
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (pressed_) {
                POINT now;
                GetCursorPos(&now);
                if (!dragging_ && (std::abs(now.x - pressedAt_.x) > kDragSlop || std::abs(now.y - pressedAt_.y) > kDragSlop)) {
                    dragging_ = true;
                }
                if (dragging_) MoveTo({pressedPlace_.x + now.x - pressedAt_.x, pressedPlace_.y + now.y - pressedAt_.y});
            }
            return 0;
        case WM_LBUTTONUP:
            if (pressed_) {
                pressed_ = false;
                ReleaseCapture();
                if (dragging_) {
                    dragging_ = false;
                    if (!KeepOnScreen(place_)) MoveTo(DefaultPlace());
                    SavePlace();
                } else {
                    Cheer();
                }
            }
            return 0;
        case WM_CAPTURECHANGED:
            pressed_ = dragging_ = false;
            return 0;
        case WM_RBUTTONUP:
            ShowMenu();
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            place_ = {suggested->left, suggested->top};
            Resize(HIWORD(wParam));
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            motion_ = AnimationsEnabled();
            if (!KeepOnScreen(place_)) MoveTo(DefaultPlace());
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    // --- shapes and drawing ------------------------------------------------

    bool BuildShapes() {
        using namespace tekito::companion;
        const auto disc = ReadPath(factory_.Get(), kDisc, false);
        body_ = Without(factory_.Get(), disc.Get(), Circle(factory_.Get(), kCutX, kCutY, kCutRadius).Get());
        ring_ = Without(factory_.Get(), Circle(factory_.Get(), kRingX, kRingY, kRingOuter).Get(),
                        Circle(factory_.Get(), kRingX, kRingY, kRingHole).Get());
        if (!disc || !body_ || !ring_) return false;
        disc->GetBounds(nullptr, &bodyBox_);
        open_ = ReadPath(factory_.Get(), kEyeOpen.path, kEyeOpen.line);
        happy_ = ReadPath(factory_.Get(), kEyeHappy.path, kEyeHappy.line);
        shut_ = ReadPath(factory_.Get(), kEyeShut.path, kEyeShut.line);
        const D2D1_STROKE_STYLE_PROPERTIES round{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                 D2D1_LINE_JOIN_ROUND};
        factory_->CreateStrokeStyle(round, nullptr, 0, &roundPen_);
        return open_ && happy_ && shut_;
    }

    void Resize(UINT dpi) {
        scale_ = static_cast<float>(dpi ? dpi : 96) / 96.0f;
        width_ = static_cast<int>(std::lround(kSize * scale_));
        height_ = static_cast<int>(std::lround(kSize * (1 + kHeadroom) * scale_));
        if (bitmap_) {
            SelectObject(memory_, previous_);
            DeleteObject(bitmap_);
        }
        if (!memory_) memory_ = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width_, -height_, 1, 32, BI_RGB};
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        previous_ = SelectObject(memory_, bitmap_);
        target_.Reset();
        Draw();
    }

    bool EnsureTarget() {
        if (target_) return true;
        // Software: a small picture, read back every frame.
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(factory_->CreateDCRenderTarget(&properties, &target_))) return false;
        target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        brushHue_ = -1.0f;
        return SUCCEEDED(target_->CreateSolidColorBrush(Color(0xffffff), &white_));
    }

    // The gradients, turned to `hue` degrees while Rainbow is on.
    void EnsureBrushes(float turn) {
        if (turn == brushHue_ && bodyBrush_) return;
        using namespace tekito::companion;
        brushHue_ = turn;
        D2D1_GRADIENT_STOP stops[std::size(kBodyStops)];
        for (std::size_t i = 0; i < std::size(kBodyStops); ++i) {
            stops[i] = {kBodyStops[i].offset, Turned(Color(kBodyStops[i].rgb), turn)};
        }
        ComPtr<ID2D1GradientStopCollection> collection;
        target_->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)), &collection);
        // In the body's box, as SVG's objectBoundingBox.
        const auto box = D2D1::Matrix3x2F::Scale(bodyBox_.right - bodyBox_.left, bodyBox_.bottom - bodyBox_.top) *
                         D2D1::Matrix3x2F::Translation(bodyBox_.left, bodyBox_.top);
        bodyBrush_.Reset();
        target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.1f, 0.05f), D2D1::Point2F(0.9f, 0.95f)),
            D2D1::BrushProperties(1.0f, box), collection.Get(), &bodyBrush_);

        D2D1_GRADIENT_STOP ring[std::size(kRingStops)];
        for (std::size_t i = 0; i < std::size(kRingStops); ++i) {
            ring[i] = {kRingStops[i].offset, Turned(Color(kRingStops[i].rgb), turn)};
        }
        collection.Reset();
        target_->CreateGradientStopCollection(ring, static_cast<UINT32>(std::size(ring)), &collection);
        const auto ringBox = D2D1::Matrix3x2F::Scale(kRingOuter * 2, kRingOuter * 2) *
                             D2D1::Matrix3x2F::Translation(kRingX - kRingOuter, kRingY - kRingOuter);
        ringBrush_.Reset();
        target_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 1)),
            D2D1::BrushProperties(1.0f, ringBox), collection.Get(), &ringBrush_);

        if (!sheenBrush_) {
            const D2D1_GRADIENT_STOP sheen[] = {{0.0f, Color(0xffffff, kSheenOpacity)}, {1.0f, Color(0xffffff, 0.0f)}};
            collection.Reset();
            target_->CreateGradientStopCollection(sheen, 2, &collection);
            target_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(kSheenX, kSheenY), D2D1::Point2F(), kSheenRadius,
                                                    kSheenRadius),
                D2D1::BrushProperties(1.0f, box), collection.Get(), &sheenBrush_);
        }
    }

    // A pair of eyes: `shape` for the left, mirrored for the right.
    void DrawEyes(ID2D1PathGeometry* shape, bool line, const D2D1_MATRIX_3X2_F& face, float size,
                  float rightSize = -1, float rightDrop = 0) {
        using namespace tekito::companion;
        const float eyes[] = {kEyeLeft, kEyeRight};
        for (int i = 0; i < 2; ++i) {
            const float s = (i && rightSize > 0 ? rightSize : size) * kEyeScale;
            const auto at = D2D1::Matrix3x2F::Scale(i ? -s : s, s) *
                            D2D1::Matrix3x2F::Translation(eyes[i] + look_.x, kEyeY + look_.y + (i ? rightDrop : 0)) * face;
            target_->SetTransform(at);
            if (line) {
                target_->DrawGeometry(shape, white_.Get(), kLinePen, roundPen_.Get());
            } else {
                target_->FillGeometry(shape, white_.Get());
            }
        }
    }

    void Draw() {
        if (!hwnd_ || hidden_ || !memory_ || !EnsureTarget()) return;
        using namespace tekito::companion;
        CheckModes();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        Follow();

        // What it shows: eyes shut in secret mode, pleased after a change,
        // wide while the pointer is on it, else open, blinking now and then.
        const bool happy = t < happyUntil_;
        const bool listening = !happy && (hover_ || dragging_);
        const bool blink = !secret_ && !happy && std::fmod(t, kBlinkPeriod) / kBlinkPeriod > 0.93;

        auto bodyMove = D2D1::Matrix3x2F::Identity();
        auto ringMove = D2D1::Matrix3x2F::Identity();
        if (motion_) {
            const double hop = t - hopStart_;
            if (hop >= 0 && hop < kHopTime * kHops) {
                const float p = static_cast<float>(std::fmod(hop, kHopTime) / kHopTime);
                float y = 0, sx = 1, sy = 1;
                if (p < 0.35f) {
                    const float k = Ease(p / 0.35f);
                    y = -kHopHeight * k, sx = 1.03f - 0.06f * k, sy = 0.97f + 0.06f * k;
                } else if (p < 0.7f) {
                    const float k = Ease((p - 0.35f) / 0.35f);
                    y = -kHopHeight * (1 - k), sx = 0.97f + 0.07f * k, sy = 1.03f - 0.07f * k;
                } else {
                    const float k = Ease((p - 0.7f) / 0.3f);
                    sx = 1.04f - 0.04f * k, sy = 0.96f + 0.04f * k;
                }
                bodyMove = D2D1::Matrix3x2F::Scale(sx, sy, D2D1::Point2F(kFootX, kFootY)) *
                           D2D1::Matrix3x2F::Translation(0, y);
            } else {
                const float rise = static_cast<float>((1 - std::cos(2 * 3.14159265 * t / kFloatPeriod)) / 2);
                bodyMove = D2D1::Matrix3x2F::Translation(0, -kFloatHeight * rise);
                if (listening) bodyMove = D2D1::Matrix3x2F::Scale(1.01f, 0.99f, D2D1::Point2F(kFootX, kFootY)) * bodyMove;
                const float bob = static_cast<float>((1 - std::cos(2 * 3.14159265 * (t - 0.4) / kFloatPeriod)) / 2);
                ringMove = D2D1::Matrix3x2F::Translation(-5 * bob, -9 * bob);
            }
        }
        const float picture = static_cast<float>(width_) / kViewSize;
        const auto view = D2D1::Matrix3x2F::Translation(-kViewLeft, -kViewLeft) *
                          D2D1::Matrix3x2F::Scale(picture, picture) *
                          D2D1::Matrix3x2F::Translation(0, static_cast<float>(height_ - width_));
        const auto face = bodyMove * view;

        const RECT bounds{0, 0, width_, height_};
        target_->BindDC(memory_, &bounds);
        target_->BeginDraw();
        target_->Clear(D2D1::ColorF(0, 0, 0, 0));
        const float turn = rainbow_ ? static_cast<float>(std::fmod(t / kRainbowPeriod, 1.0) * 360.0) : 0.0f;
        EnsureBrushes(rainbow_ ? std::round(turn) : 0.0f);
        target_->SetTransform(face);
        target_->FillGeometry(body_.Get(), bodyBrush_.Get());
        target_->FillGeometry(body_.Get(), sheenBrush_.Get());
        target_->SetTransform(ringMove * face);
        target_->FillGeometry(ring_.Get(), ringBrush_.Get());
        if (secret_ || blink) {
            DrawEyes(shut_.Get(), true, face, 1.0f);
        } else if (happy) {
            DrawEyes(happy_.Get(), false, face, 1.0f);
        } else {
            DrawEyes(open_.Get(), false, face, listening ? kListeningEyes : 1.0f);
        }
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
            target_.Reset();
            bodyBrush_.Reset();
            ringBrush_.Reset();
            sheenBrush_.Reset();
            white_.Reset();
            return;
        }
        Show();
    }

    void Show() {
        POINT source{0, 0};
        SIZE size{width_, height_};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, nullptr, &place_, &size, memory_, &source, 0, &blend, ULW_ALPHA);
    }

    // The eyes toward the pointer (a glance, eased), and whether it is on
    // the body.
    void Follow() {
        POINT pointer;
        GetCursorPos(&pointer);
        const float cx = static_cast<float>(place_.x) + width_ / 2.0f;
        const float cy = static_cast<float>(place_.y + height_) - width_ / 2.0f;
        const float dx = static_cast<float>(pointer.x) - cx;
        const float dy = static_cast<float>(pointer.y) - cy;
        const float distance = std::max(std::hypot(dx, dy), 1.0f);
        hover_ = distance < width_ * 0.45f;
        const float reach = std::min(1.0f, distance / (240.0f * scale_)) * kLookReach;
        const D2D1_POINT_2F aim{dx / distance * reach, dy / distance * reach * 0.8f};
        look_.x += (aim.x - look_.x) * 0.25f;
        look_.y += (aim.y - look_.y) * 0.25f;
    }

    void Cheer() {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        happyUntil_ = t + kHappyTime;
        hopStart_ = t;
    }

    // --- settings, modes, full screen --------------------------------------

    void Check() {
        if (runtime_->SettingsGeneration() != settingsGeneration_) {
            settingsGeneration_ = runtime_->SettingsGeneration();
            tekito::userdata::UserSettings loaded;
            if (repository_->LoadSettings(loaded)) settings_ = loaded;
            if (!settings_.desktopCompanion && !preview_) {
                DestroyWindow(hwnd_);
                return;
            }
        }
        rainbow_ = settings_.rainbowAccent;
        if (ScreenBusy()) {
            if (!hidden_) {
                hidden_ = true;
                frameTimer_ = false;
                KillTimer(hwnd_, kFrameTimer);
                ShowWindow(hwnd_, SW_HIDE);
            }
        } else if (hidden_ || !frameTimer_) {
            hidden_ = false;
            frameTimer_ = SetTimer(hwnd_, kFrameTimer, kFrameMilliseconds, nullptr) != 0;
            Draw();
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
    }

    void CheckModes() {
        secret_ = runtime_->Secret();
        if (runtime_->ModeGeneration() != modeGeneration_) {
            modeGeneration_ = runtime_->ModeGeneration();
            Cheer();
        }
    }

    void ShowMenu() {
        const bool japanese = tekito::userdata::UseJapaneseUi(settings_.uiLanguage);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuSettings, japanese ? L"設定を開く" : L"Open Settings");
        AppendMenuW(menu, MF_STRING, kMenuHide, japanese ? L"コンパニオンを非表示にする" : L"Hide the companion");
        POINT at;
        GetCursorPos(&at);
        SetForegroundWindow(hwnd_);
        const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (chosen == kMenuSettings) {
            std::wstring command = L"\"" + AppPath() + L"\"";
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        } else if (chosen == kMenuHide) {
            // The setting off, as from Settings.
            tekito::userdata::UserSettings current;
            if (repository_->LoadSettings(current)) {
                current.desktopCompanion = false;
                if (repository_->SaveSettings(current)) runtime_->NotifySettingsChanged();
            }
            SetCompanionAtStartup(false);
            DestroyWindow(hwnd_);
        }
    }

    // --- where it is -------------------------------------------------------

    void MoveTo(POINT at) {
        place_ = at;
        SetWindowPos(hwnd_, nullptr, place_.x, place_.y, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
        Show();
    }

    // Whether `at` keeps it on a screen; moves it there if so.
    bool KeepOnScreen(POINT at) {
        const RECT rect{at.x, at.y, at.x + width_, at.y + height_};
        if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) return false;
        MoveTo(at);
        return true;
    }

    // The bottom right of the main screen, above the taskbar.
    POINT DefaultPlace() const {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int gap = static_cast<int>(std::lround(kCornerGap * scale_));
        return {work.right - width_ - gap, work.bottom - height_ - gap};
    }

    POINT SavedPlace() {
        saved_ = {LONG_MIN, LONG_MIN};
        DWORD x = 0, y = 0, size = sizeof(DWORD);
        if (RegGetValueW(HKEY_CURRENT_USER, kPlaceKey, L"Left", RRF_RT_REG_DWORD, nullptr, &x, &size) == ERROR_SUCCESS &&
            (size = sizeof(DWORD), RegGetValueW(HKEY_CURRENT_USER, kPlaceKey, L"Top", RRF_RT_REG_DWORD, nullptr, &y,
                                                &size) == ERROR_SUCCESS)) {
            saved_ = {static_cast<LONG>(static_cast<int>(x)), static_cast<LONG>(static_cast<int>(y))};
        }
        return saved_.x == LONG_MIN ? POINT{0, 0} : saved_;
    }

    void SavePlace() const {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kPlaceKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            return;
        }
        const DWORD x = static_cast<DWORD>(place_.x);
        const DWORD y = static_cast<DWORD>(place_.y);
        RegSetValueExW(key, L"Left", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&x), sizeof(x));
        RegSetValueExW(key, L"Top", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&y), sizeof(y));
        RegCloseKey(key);
    }

    HINSTANCE instance_{nullptr};
    HWND hwnd_{nullptr};
    std::unique_ptr<tekito::userdata::IUserDataRepository> repository_;
    std::unique_ptr<tekito::userdata::RuntimeModeState> runtime_;
    tekito::userdata::UserSettings settings_;
    std::uint32_t modeGeneration_{0};
    std::uint32_t settingsGeneration_{0};

    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1DCRenderTarget> target_;
    ComPtr<ID2D1PathGeometry> body_;
    ComPtr<ID2D1PathGeometry> ring_;
    ComPtr<ID2D1PathGeometry> open_;
    ComPtr<ID2D1PathGeometry> happy_;
    ComPtr<ID2D1PathGeometry> shut_;
    ComPtr<ID2D1StrokeStyle> roundPen_;
    ComPtr<ID2D1LinearGradientBrush> bodyBrush_;
    ComPtr<ID2D1LinearGradientBrush> ringBrush_;
    ComPtr<ID2D1RadialGradientBrush> sheenBrush_;
    ComPtr<ID2D1SolidColorBrush> white_;
    D2D1_RECT_F bodyBox_{};
    float brushHue_{-1.0f};

    HDC memory_{nullptr};
    HBITMAP bitmap_{nullptr};
    HGDIOBJ previous_{nullptr};
    float scale_{1.0f};
    int width_{0};
    int height_{0};
    POINT place_{};
    POINT saved_{};

    std::chrono::steady_clock::time_point started_{};
    double happyUntil_{-1.0};
    double hopStart_{-100.0};
    D2D1_POINT_2F look_{};
    bool preview_{false};
    bool motion_{true};
    bool rainbow_{false};
    bool secret_{false};
    bool hover_{false};
    bool hidden_{false};
    bool frameTimer_{false};
    bool pressed_{false};
    bool dragging_{false};
    POINT pressedAt_{};
    POINT pressedPlace_{};
};

}  // namespace

std::optional<int> RunCompanionFromCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool companion = false;
    for (int i = 1; arguments && i < count; ++i) companion = companion || std::wstring_view(arguments[i]) == L"--companion";
    if (arguments) LocalFree(arguments);
    if (!companion) return std::nullopt;

    HANDLE single = CreateMutexW(nullptr, TRUE, kSingleInstance);
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single) CloseHandle(single);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int result = 0;
    {
        Companion companionWindow;
        // Not created when the setting is off (left in Windows' startup list).
        if (companionWindow.Create(GetModuleHandleW(nullptr))) result = companionWindow.Run();
    }
    ReleaseMutex(single);
    CloseHandle(single);
    return result;
}

void StartCompanion() {
    if (FindWindowW(kWindowClass, nullptr)) return;
    std::wstring command = L"\"" + AppPath() + L"\" --companion";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void SetCompanionAtStartup(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        const std::wstring command = L"\"" + AppPath() + L"\" --companion";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}
