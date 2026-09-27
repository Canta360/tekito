// C++/WinRT needs the classic COM declarations before its own headers so
// com_ptr/as<> can interoperate with the composition interop interfaces.
#include <unknwn.h>

#include "Tsf/CandidateWindow.h"

#include "assets/tekito_resource.h"

#include <DispatcherQueue.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <windows.foundation.h>
#include <windows.graphics.effects.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <winrt/Windows.UI.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace tekito::tsf {
namespace {

namespace wuc = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

constexpr wchar_t kWindowClassName[] = L"TekitoCandidateWindow";
constexpr wchar_t kShadowClassName[] = L"TekitoCandidateShadow";
constexpr wchar_t kControlClassName[] = L"TekitoCandidateControl";
constexpr wchar_t kNotifyClassName[] = L"TekitoCandidateNotify";

constexpr UINT kMsgUpdate = WM_APP + 1;
constexpr UINT kMsgQuit = WM_APP + 2;
constexpr UINT kMsgSelect = WM_APP + 3;

constexpr UINT kBaseDpi = 96;

// Layout, in DIPs. Every corner is concentric: the selection capsule is a
// full pill (radius = half the row height) and the panel corner is that
// radius plus the padding around the capsule, so the two curves stay
// parallel the way nested glass shapes do.
constexpr int kRowHeight = 36;
constexpr int kRowGap = 2;
constexpr int kPanelPadding = 6;
constexpr int kCornerRadius = kRowHeight / 2 + kPanelPadding;
// DWMWCP_ROUND radius, used when composition is unavailable and DWM clips.
constexpr int kFallbackCornerRadius = 8;
constexpr int kRowPaddingLeft = 12;
constexpr int kNumberWidth = 16;
constexpr int kNumberGap = 10;
constexpr int kRowPaddingRight = 14;
constexpr int kTagGap = 10;
constexpr int kTagPaddingX = 7;
constexpr int kTagHeight = 18;
constexpr float kTagTracking = 0.6f;
constexpr int kIndicatorGutter = 8;
constexpr int kIndicatorWidth = 3;
constexpr int kIndicatorInset = 14;
constexpr int kMinWidth = 200;
constexpr int kMaxWidth = 440;
// The meaning pane beside the list: a fixed width, as tall as the list or
// its text (up to kDetailMaxHeight, dropping later senses to fit).
constexpr int kDetailWidth = 250;
constexpr int kDetailPadding = 14;
constexpr int kDetailMaxHeight = 300;
constexpr int kCaretGap = 6;
constexpr int kSelectionPad = 8;
constexpr int kGlowDepth = 9;
constexpr int kShadeDepth = 4;
constexpr int kEdgeDepth = 2;
constexpr int kRecessDepth = 4;
constexpr int kLightSpread = 22;

constexpr float kFontCandidate = 15.0f;
constexpr float kFontNumber = 12.5f;
constexpr float kFontTag = 9.5f;
constexpr float kFontDetail = 12.5f;
constexpr float kFontDetailHead = 15.0f;

// Floating shadow: a tight contact shadow for edge definition plus a soft
// ambient one for lift. It lives in a separate click-through window so the
// area it covers does not swallow clicks meant for the app underneath.
constexpr float kShadowKeyBlur = 1.2f;
constexpr float kShadowKeyOffset = 1.0f;
constexpr float kShadowAmbientBlur = 7.0f;
constexpr float kShadowAmbientOffset = 6.0f;
constexpr int kShadowMarginX = 16;
constexpr int kShadowMarginTop = 10;
constexpr int kShadowMarginBottom = 26;

constexpr auto kGlideDuration = std::chrono::milliseconds(180);

// A little extra color in the blurred backdrop keeps the frosted glass from
// looking gray and dead without making it a clear window.
constexpr float kBackdropSaturation = 1.3f;

// Where light catches a glass edge: a faint line all the way round, a hot
// spot on the top-left curve and a weaker one diagonally opposite, as if lit
// from the upper left.
struct Specular {
    float base;
    float topLeft;
    float bottomRight;
};

struct Palette {
    D2D1_COLOR_F solid;       // panel base where no backdrop blur is available
    D2D1_COLOR_F tintTop;     // frosted veil over the blur, top edge
    D2D1_COLOR_F tintBottom;  // ... and bottom edge
    D2D1_COLOR_F glow;        // luminous band just inside the edge
    D2D1_COLOR_F shade;       // darker band inside the bottom edge
    D2D1_COLOR_F edge;        // thin dark ring inside the rim: the thickness of the slab
    D2D1_COLOR_F gleam;       // soft light pooled in the top-left of the panel
    D2D1_COLOR_F rim;         // specular edge color
    Specular rimLight;
    float rimBottom;          // bright lip along the bottom edge
    D2D1_COLOR_F text;        // unselected rows sit back a little ...
    D2D1_COLOR_F mutedText;
    D2D1_COLOR_F tagFill;
    D2D1_COLOR_F indicator;
    float shadowKey;
    float shadowAmbient;
};

constexpr Palette kLightPalette{
    {0.953f, 0.957f, 0.965f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.56f},
    {1.0f, 1.0f, 1.0f, 0.42f},
    {1.0f, 1.0f, 1.0f, 0.50f},
    {0.0f, 0.0f, 0.0f, 0.05f},
    {0.0f, 0.0f, 0.0f, 0.06f},
    {1.0f, 1.0f, 1.0f, 0.20f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.30f, 1.0f, 0.60f},
    0.90f,
    {0.180f, 0.196f, 0.231f, 1.0f},
    {0.330f, 0.357f, 0.404f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.06f},
    {0.0f, 0.0f, 0.0f, 0.28f},
    0.14f,
    0.16f,
};

constexpr Palette kDarkPalette{
    {0.110f, 0.118f, 0.133f, 1.0f},
    {0.130f, 0.140f, 0.160f, 0.62f},
    {0.070f, 0.080f, 0.090f, 0.72f},
    {1.0f, 1.0f, 1.0f, 0.08f},
    {0.0f, 0.0f, 0.0f, 0.25f},
    {0.0f, 0.0f, 0.0f, 0.35f},
    {1.0f, 1.0f, 1.0f, 0.05f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.10f, 0.50f, 0.25f},
    0.40f,
    {0.780f, 0.792f, 0.816f, 1.0f},
    {0.530f, 0.557f, 0.604f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.08f},
    {1.0f, 1.0f, 1.0f, 0.36f},
    0.35f,
    0.40f,
};

// The selection is a pill pressed into the glass slab: a little darker than
// the panel, shadowed under its top edge and catching light on its lower
// lip. Under the frosted glass beneath it sits a soft pool of accent light,
// diffused by the frost, which follows the selection from row to row. The
// selected word is simply brighter (dark: whiter, light: blacker) than the
// rest.
struct SelectionStyle {
    D2D1_COLOR_F fill;
    D2D1_COLOR_F text;
    D2D1_COLOR_F mutedText;
    D2D1_COLOR_F tagFill;
    D2D1_COLOR_F recess;  // inner shadow below the pill's top edge
    float rimBottom;      // light on the pill's lower lip
    float light;          // peak of the accent light under the glass
    bool glass;
};

constexpr SelectionStyle kLightSelection{
    {0.10f, 0.12f, 0.16f, 0.07f},
    {0.020f, 0.024f, 0.031f, 1.0f},
    {0.200f, 0.216f, 0.251f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.07f},
    {0.0f, 0.0f, 0.0f, 0.10f},
    0.95f,
    0.16f,
    true,
};

constexpr SelectionStyle kDarkSelection{
    {0.0f, 0.0f, 0.0f, 0.30f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.800f, 0.816f, 0.840f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.10f},
    {0.0f, 0.0f, 0.0f, 0.45f},
    0.22f,
    0.50f,
    true,
};
// "Simple" style: the first Direct2D design. An opaque panel with a thin
// border, a flat tinted selection row marked by a short accent rail, and no
// glass effects.
constexpr Palette kSimpleLightPalette{
    {1.0f, 1.0f, 1.0f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.863f, 0.898f, 0.945f, 1.0f},
    {1.0f, 0.0f, 0.0f},
    0.0f,
    {0.110f, 0.137f, 0.184f, 1.0f},
    {0.400f, 0.439f, 0.502f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.28f},
    0.10f,
    0.12f,
};

constexpr Palette kSimpleDarkPalette{
    {0.118f, 0.125f, 0.141f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.0f},
    {0.235f, 0.247f, 0.278f, 1.0f},
    {1.0f, 0.0f, 0.0f},
    0.0f,
    {0.902f, 0.910f, 0.925f, 1.0f},
    {0.588f, 0.612f, 0.655f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 0.36f},
    0.35f,
    0.40f,
};

constexpr SelectionStyle kSimpleLightSelection{
    {0.933f, 0.969f, 1.0f, 1.0f},
    kSimpleLightPalette.text,
    kSimpleLightPalette.mutedText,
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    0.0f,
    0.0f,
    false,
};

constexpr SelectionStyle kSimpleDarkSelection{
    {0.176f, 0.227f, 0.314f, 1.0f},
    kSimpleDarkPalette.text,
    kSimpleDarkPalette.mutedText,
    {0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f},
    0.0f,
    0.0f,
    false,
};

constexpr int kSimpleCornerRadius = 12;
constexpr int kSimpleRowRadius = 8;
constexpr int kSimpleRailWidth = 3;

struct Style {
    Palette palette{kLightPalette};
    SelectionStyle selection{};
    D2D1_COLOR_F light{0.0f, 0.471f, 0.831f, 1.0f};  // color of the light under the selection
    D2D1_COLOR_F rail{0.024f, 0.396f, 1.0f, 1.0f};    // simple style: selection marker
    bool glass{false};
    bool simple{false};
    bool animations{true};
};

struct Row {
    std::wstring number;
    std::wstring text;
    std::wstring label;
};

constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

struct Frame {
    bool visible{false};
    RECT caret{};
    std::vector<Row> rows;
    std::size_t selectedRow{kNoRow};
    std::size_t firstIndex{0};
    std::size_t totalCount{0};
    std::uint64_t generation{0};
    int style{0};  // CandidateWindow::Style
    std::wstring detailHead;
    std::vector<std::wstring> detailSenses;
};

}  // namespace

// Host thread -> UI thread hand-off. Only the latest frame matters, so
// repeated Show calls overwrite `pending` and the UI thread coalesces them.
struct CandidateWindow::Channel {
    ~Channel() {
        if (ready) CloseHandle(ready);
    }

    std::mutex mutex;
    Frame pending;
    bool hasPending{false};
    HWND controlHwnd{nullptr};
    HWND notifyHwnd{nullptr};
    HINSTANCE instance{nullptr};
    DPI_AWARENESS_CONTEXT dpiContext{nullptr};
    HANDLE ready{nullptr};
};

namespace {

DWORD ReadPersonalizeValue(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    const auto result = RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return result == ERROR_SUCCESS ? value : fallback;
}

bool IsDarkThemeActive() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    wchar_t theme[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_THEME", theme, 16) > 0) {
        return wcscmp(theme, L"dark") == 0;
    }
#endif
    return ReadPersonalizeValue(L"AppsUseLightTheme", 1) == 0;
}

// Settings > Personalization > Colors > Transparency effects.
bool IsTransparencyEnabled() {
    return ReadPersonalizeValue(L"EnableTransparency", 1) != 0;
}

bool IsHighContrast() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_HIGH_CONTRAST", nullptr, 0) > 0) return true;
#endif
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// Settings > Accessibility > Visual effects > Animation effects.
bool AreAnimationsEnabled() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled != FALSE;
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}

D2D1_COLOR_F SystemColor(int index) {
    const COLORREF color = GetSysColor(index);
    return {GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, 1.0f};
}

// The light under the selection takes the user's Windows accent color: a
// brighter shade on dark glass, the base shade on light glass.
D2D1_COLOR_F AccentLight(bool dark) {
    try {
        using winrt::Windows::UI::ViewManagement::UIColorType;
        const auto color = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            dark ? UIColorType::AccentLight1 : UIColorType::Accent);
        return {color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, 1.0f};
    } catch (...) {
        return dark ? D2D1_COLOR_F{0.298f, 0.694f, 1.0f, 1.0f} : D2D1_COLOR_F{0.0f, 0.471f, 0.831f, 1.0f};
    }
}

Style ReadStyle(bool compositionGlass, bool simple) {
    Style style;
    style.animations = AreAnimationsEnabled();
    style.simple = simple;
    if (IsHighContrast()) {
        // High contrast wants the user's exact system colors and no effects.
        Palette& p = style.palette;
        p.solid = SystemColor(COLOR_WINDOW);
        p.tintTop = p.tintBottom = p.glow = p.shade = p.edge = p.gleam = WithAlpha(p.solid, 0.0f);
        p.rim = SystemColor(COLOR_WINDOWTEXT);
        p.rimLight = {1.0f, 0.0f, 0.0f};
        p.rimBottom = 0.0f;
        p.text = p.mutedText = p.indicator = SystemColor(COLOR_WINDOWTEXT);
        p.tagFill = WithAlpha(p.solid, 0.0f);
        p.shadowKey = p.shadowAmbient = 0.0f;
        const auto highlightText = SystemColor(COLOR_HIGHLIGHTTEXT);
        const auto none = WithAlpha(highlightText, 0.0f);
        style.selection = {SystemColor(COLOR_HIGHLIGHT), highlightText, highlightText, none, none,
                           0.0f, 0.0f, false};
        style.glass = false;
        return style;
    }
    const bool dark = IsDarkThemeActive();
    if (simple) {
        style.palette = dark ? kSimpleDarkPalette : kSimpleLightPalette;
        style.selection = dark ? kSimpleDarkSelection : kSimpleLightSelection;
        style.rail = dark ? D2D1_COLOR_F{0.353f, 0.627f, 1.0f, 1.0f} : D2D1_COLOR_F{0.024f, 0.396f, 1.0f, 1.0f};
        style.glass = false;
        return style;
    }
    style.palette = dark ? kDarkPalette : kLightPalette;
    style.selection = dark ? kDarkSelection : kLightSelection;
    style.light = AccentLight(dark);
    style.glass = compositionGlass && IsTransparencyEnabled();
    return style;
}

int ScaleForWindow(HWND hwnd, int value) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : kBaseDpi;
    return MulDiv(value, dpi == 0 ? kBaseDpi : dpi, kBaseDpi);
}

float ScaleFactor(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : kBaseDpi;
    return static_cast<float>(dpi == 0 ? kBaseDpi : dpi) / static_cast<float>(kBaseDpi);
}

RECT WorkAreaNear(const RECT& rect) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcWork;
    }

    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        return work;
    }
    return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

POINT CandidatePosition(const RECT& caretRect, int width, int height, int caretGap) {
    const RECT work = WorkAreaNear(caretRect);

    int x = caretRect.left;
    if (x + width > work.right) x = work.right - width;
    if (x < work.left) x = work.left;

    int y = caretRect.bottom + caretGap;
    if (y + height > work.bottom) y = caretRect.top - height - caretGap;
    if (y < work.top) y = work.top;

    return {x, y};
}

std::wstring LabelText(const Candidate& candidate, bool japanese) {
    // Deliberately minimal: a label only earns a place here if knowing it
    // changes whether the user would pick this candidate. ORIGINAL says
    // "nothing was changed"; SLANG says "this is an informal substitution,
    // not a spelling fix"; EMOJI disambiguates a glyph that can be hard to
    // read at candidate-row size. Every other SemanticLabel (proper noun,
    // phrase, reaction, abbreviation, standard correction, ...) explains
    // *why* the engine generated the candidate, not whether the user should
    // want it, so it stays unlabeled to keep the row scannable.
    if (candidate.isOriginal) return japanese ? L"そのまま" : L"ORIGINAL";
    if (candidate.label == SemanticLabel::Slang) return japanese ? L"スラング" : L"SLANG";
    if (candidate.label == SemanticLabel::Emoji) return japanese ? L"絵文字" : L"EMOJI";
    return {};
}

// ---------------------------------------------------------------------------
// Layout and drawing. Everything is in physical pixels (D2D at 96 DPI), with
// DIP constants scaled by the window DPI, so the same routines serve the
// composition surfaces and the HWND fallback.

struct TextFormats {
    UINT dpi{0};
    winrt::com_ptr<IDWriteTextFormat> text;
    winrt::com_ptr<IDWriteTextFormat> textSelected;
    winrt::com_ptr<IDWriteTextFormat> number;
    winrt::com_ptr<IDWriteTextFormat> tag;
    winrt::com_ptr<IDWriteTextFormat> detail;
};

struct Layout {
    int width{0};
    int height{0};
    float scale{1.0f};
    float radius{0.0f};
    float padding{0.0f};
    float rowHeight{0.0f};
    float rowPitch{0.0f};
    float rowPaddingLeft{0.0f};
    float numberWidth{0.0f};
    float numberGap{0.0f};
    float rowPaddingRight{0.0f};
    float tagGap{0.0f};
    float tagPaddingX{0.0f};
    float tagHeight{0.0f};
    float tagTracking{0.0f};
    float rowRight{0.0f};
    // The list part; the rest of the width is the meaning pane.
    float listWidth{0.0f};
    float listHeight{0.0f};
    D2D1_RECT_F detail{};
    winrt::com_ptr<IDWriteTextLayout> detailText;
    bool indicator{false};
    float indicatorWidth{0.0f};
    float indicatorInset{0.0f};

    [[nodiscard]] D2D1_RECT_F RowRect(std::size_t row) const {
        const float top = padding + static_cast<float>(row) * rowPitch;
        return D2D1::RectF(padding, top, rowRight, top + rowHeight);
    }
};

D2D1_RECT_F Inset(const D2D1_RECT_F& rect, float amount) {
    return D2D1::RectF(rect.left + amount, rect.top + amount, rect.right - amount,
                       rect.bottom - amount);
}

D2D1_ROUNDED_RECT Rounded(const D2D1_RECT_F& rect, float radius) {
    const float r = std::max(radius, 0.0f);
    return D2D1::RoundedRect(rect, r, r);
}

winrt::com_ptr<ID2D1SolidColorBrush> SolidBrush(ID2D1RenderTarget* target, const D2D1_COLOR_F& color) {
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(color, brush.put());
    return brush;
}

winrt::com_ptr<ID2D1LinearGradientBrush> GradientBrush(ID2D1RenderTarget* target,
                                                       D2D1_POINT_2F from, D2D1_POINT_2F to,
                                                       const D2D1_GRADIENT_STOP* stops,
                                                       UINT32 count) {
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> brush;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, count, collection.put()))) {
        target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to),
                                          collection.get(), brush.put());
    }
    return brush;
}

// The TEKITO typeface (M PLUS 1) ships inside this DLL. It is loaded into a
// private collection on the candidate window's own DirectWrite factory, so
// the host application's fonts are never touched.
struct BrandFont {
    winrt::com_ptr<IDWriteInMemoryFontFileLoader> loader;
    winrt::com_ptr<IDWriteFontCollection1> collection;
};

BrandFont LoadBrandFont(IDWriteFactory* factory, HINSTANCE module) {
    BrandFont font;
    winrt::com_ptr<IDWriteFactory5> factory5;
    if (factory) factory->QueryInterface(IID_PPV_ARGS(factory5.put()));
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_FONT_MPLUS1), RT_RCDATA);
    const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    const void* bytes = loaded && size ? LockResource(loaded) : nullptr;
    if (!factory5 || !bytes) return font;
    winrt::com_ptr<IDWriteFontFile> file;
    winrt::com_ptr<IDWriteFontSetBuilder1> builder;
    winrt::com_ptr<IDWriteFontSet> set;
    if (FAILED(factory5->CreateInMemoryFontFileLoader(font.loader.put())) ||
        FAILED(factory5->RegisterFontFileLoader(font.loader.get()))) {
        font.loader = nullptr;
        return font;
    }
    // The resource stays mapped for as long as the module is loaded, so the
    // loader can reference it without a copy.
    if (FAILED(font.loader->CreateInMemoryFontFileReference(factory5.get(), bytes, size, nullptr, file.put())) ||
        FAILED(factory5->CreateFontSetBuilder(builder.put())) ||
        FAILED(builder->AddFontFile(file.get())) || FAILED(builder->CreateFontSet(set.put())) ||
        FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), font.collection.put()))) {
        font.collection = nullptr;
    }
    return font;
}

winrt::com_ptr<IDWriteTextFormat> CreateFormat(IDWriteFactory* factory, IDWriteFontCollection* brand,
                                               float size, DWRITE_FONT_WEIGHT weight,
                                               DWRITE_TEXT_ALIGNMENT alignment, bool ellipsis) {
    winrt::com_ptr<IDWriteTextFormat> format;
    // Japanese locale: kanji and kana that M PLUS 1 lacks fall back to a
    // Japanese font, with Japanese glyph shapes. Latin text is unaffected.
    if (FAILED(factory->CreateTextFormat(brand ? L"M PLUS 1" : L"Segoe UI Variable Text", brand, weight,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         size, L"ja-jp", format.put()))) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    if (ellipsis) {
        // Over-long candidates end in "..." instead of being cut mid-glyph.
        winrt::com_ptr<IDWriteInlineObject> sign;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format.get(), sign.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, sign.get());
        }
    }
    return format;
}

float MeasureTextWidth(IDWriteFactory* factory, IDWriteTextFormat* format, const std::wstring& text) {
    if (text.empty() || !factory || !format) return 0.0f;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                         10000.0f, 1000.0f, layout.put()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

// Small-caps style tag text with a little tracking, split evenly before and
// after each glyph so the word stays centered in its capsule.
winrt::com_ptr<IDWriteTextLayout> TagLayout(IDWriteFactory* factory, IDWriteTextFormat* format,
                                            const std::wstring& text, float height, float tracking) {
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!factory || !format ||
        FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                         1000.0f, height, layout.put()))) {
        return nullptr;
    }
    if (const auto layout1 = layout.try_as<IDWriteTextLayout1>()) {
        layout1->SetCharacterSpacing(tracking / 2.0f, tracking / 2.0f, 0.0f,
                                     {0, static_cast<UINT32>(text.size())});
    }
    return layout;
}

float LayoutWidth(IDWriteTextLayout* layout) {
    if (!layout) return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

// A soft band along the inside of a rounded shape, drawn as concentric 1px
// rings whose opacity falls off like a Gaussian away from the edge. `brush`
// carries the color and any directional weighting (e.g. top-heavy).
void InnerBand(ID2D1RenderTarget* target, const D2D1_RECT_F& shape, float radius, float scale,
               int depthDips, float peak, const winrt::com_ptr<ID2D1LinearGradientBrush>& brush) {
    if (!brush || peak <= 0.0f) return;
    const int depth = std::max(1, static_cast<int>(std::lround(static_cast<float>(depthDips) * scale)));
    const float sigma = static_cast<float>(depth) / 2.6f;
    for (int ring = 0; ring < depth; ++ring) {
        const float inset = static_cast<float>(ring) + 0.5f;
        const float falloff = std::exp(-static_cast<float>(ring * ring) / (2.0f * sigma * sigma));
        brush->SetOpacity(peak * falloff);
        target->DrawRoundedRectangle(Rounded(Inset(shape, inset), radius - inset), brush.get(), 1.0f);
    }
}

// A soft pool of light: `color` at its own alpha at `center`, fading to
// nothing at `reach`.
winrt::com_ptr<ID2D1RadialGradientBrush> Hotspot(ID2D1RenderTarget* target, D2D1_POINT_2F center,
                                                 float reach, const D2D1_COLOR_F& color) {
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, color},
                                        {0.4f, WithAlpha(color, color.a * 0.4f)},
                                        {1.0f, WithAlpha(color, 0.0f)}};
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1RadialGradientBrush> brush;
    if (SUCCEEDED(target->CreateGradientStopCollection(stops, 3, collection.put()))) {
        target->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(0.0f, 0.0f), reach, reach),
            collection.get(), brush.put());
    }
    return brush;
}

// The edge of a glass shape catching light; see Specular.
void SpecularRim(ID2D1RenderTarget* target, const D2D1_RECT_F& shape, float radius, float scale,
                 const D2D1_COLOR_F& color, const Specular& light) {
    const float width = std::max(1.0f, std::floor(scale));
    const auto edge = Rounded(Inset(shape, width / 2.0f), radius - width / 2.0f);
    if (light.base > 0.0f) {
        auto base = SolidBrush(target, WithAlpha(color, light.base));
        target->DrawRoundedRectangle(edge, base.get(), width);
    }
    const float reach = 2.5f * radius + 0.35f * std::min(shape.right - shape.left, shape.bottom - shape.top);
    const float inset = 0.3f * radius;
    if (light.topLeft > 0.0f) {
        if (auto glint = Hotspot(target, D2D1::Point2F(shape.left + inset, shape.top + inset), reach,
                                 WithAlpha(color, light.topLeft))) {
            target->DrawRoundedRectangle(edge, glint.get(), width);
        }
    }
    if (light.bottomRight > 0.0f) {
        if (auto glint = Hotspot(target, D2D1::Point2F(shape.right - inset, shape.bottom - inset),
                                 0.8f * reach, WithAlpha(color, light.bottomRight))) {
            target->DrawRoundedRectangle(edge, glint.get(), width);
        }
    }
}
// Light caught along the lower edge of a glass shape, fading out up its sides.
void BottomLip(ID2D1RenderTarget* target, const D2D1_RECT_F& shape, float radius, float scale,
               const D2D1_COLOR_F& color) {
    if (color.a <= 0.0f) return;
    const float width = std::max(1.0f, std::floor(scale));
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, WithAlpha(color, 0.0f)},
                                        {0.55f, WithAlpha(color, 0.0f)},
                                        {1.0f, color}};
    if (auto lip = GradientBrush(target, D2D1::Point2F(0.0f, shape.top), D2D1::Point2F(0.0f, shape.bottom),
                                 stops, 3)) {
        target->DrawRoundedRectangle(Rounded(Inset(shape, width / 2.0f), radius - width / 2.0f), lip.get(),
                                     width);
    }
}

// The glass body: translucent veil, luminous inner edge and specular rim.
// `glass` is false when there is no blur underneath (transparency effects
// off, high contrast, or the HWND fallback), which drops the glow.
void DrawChrome(ID2D1RenderTarget* target, const Layout& layout, const Style& style) {
    const Palette& p = style.palette;
    const float width = static_cast<float>(layout.width);
    const float height = static_cast<float>(layout.height);
    const D2D1_RECT_F panel = D2D1::RectF(0.0f, 0.0f, width, height);

    const D2D1_GRADIENT_STOP tintStops[] = {{0.0f, p.tintTop}, {1.0f, p.tintBottom}};
    if (auto tint = GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                  tintStops, 2)) {
        target->FillRoundedRectangle(Rounded(panel, layout.radius), tint.get());
    }

    if (style.glass) {
        // Thickness: a band of light just inside the edge, strongest at the
        // top, and a faint darker band inside the bottom edge.
        const D2D1_GRADIENT_STOP glowStops[] = {{0.0f, WithAlpha(p.glow, 1.0f)},
                                                {1.0f, WithAlpha(p.glow, 0.35f)}};
        const D2D1_GRADIENT_STOP shadeStops[] = {{0.0f, WithAlpha(p.shade, 0.0f)},
                                                 {0.55f, WithAlpha(p.shade, 0.0f)},
                                                 {1.0f, WithAlpha(p.shade, 1.0f)}};
        InnerBand(target, panel, layout.radius, layout.scale, kGlowDepth, p.glow.a,
                  GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                glowStops, 2));
        InnerBand(target, panel, layout.radius, layout.scale, kShadeDepth, p.shade.a,
                  GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                shadeStops, 3));
    }

    if (style.glass && p.gleam.a > 0.0f) {
        if (auto gleam = Hotspot(target, D2D1::Point2F(0.0f, 0.0f),
                                 0.9f * std::min(width, height) + layout.radius, p.gleam)) {
            target->FillRoundedRectangle(Rounded(panel, layout.radius), gleam.get());
        }
    }

    if (p.edge.a > 0.0f) {
        const D2D1_GRADIENT_STOP edgeStops[] = {{0.0f, WithAlpha(p.edge, 1.0f)},
                                                {1.0f, WithAlpha(p.edge, 1.0f)}};
        InnerBand(target, panel, layout.radius, layout.scale, kEdgeDepth, p.edge.a,
                  GradientBrush(target, D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, height),
                                edgeStops, 2));
    }

    SpecularRim(target, panel, layout.radius, layout.scale, p.rim, p.rimLight);
    BottomLip(target, panel, layout.radius, layout.scale, WithAlpha(p.rim, p.rimBottom));
}

// Soft pool of accent light under the frosted glass, centered on the
// selected row. Drawn below the veil so the frost diffuses it.
void DrawLight(ID2D1RenderTarget* target, const D2D1_RECT_F& capsule, float scale, const Style& style) {
    const float peak = style.selection.light;
    if (peak <= 0.0f) return;
    const float radius = (capsule.bottom - capsule.top) / 2.0f;
    auto light = SolidBrush(target, WithAlpha(style.light, 1.0f));
    light->SetOpacity(peak);
    target->FillRoundedRectangle(Rounded(capsule, radius), light.get());
    const int spread = std::max(1, static_cast<int>(std::lround(kLightSpread * scale)));
    const float sigma = static_cast<float>(spread) / 2.6f;
    for (int ring = 0; ring < spread; ++ring) {
        const float outset = static_cast<float>(ring) + 0.5f;
        const float falloff = std::exp(-static_cast<float>(ring * ring) / (2.0f * sigma * sigma));
        light->SetOpacity(peak * falloff);
        target->DrawRoundedRectangle(Rounded(Inset(capsule, -outset), radius + outset), light.get(), 1.0f);
    }
}

// The selection pill: see SelectionStyle.
void DrawSelection(ID2D1RenderTarget* target, const D2D1_RECT_F& capsule, float scale,
                   const Style& style) {
    const SelectionStyle& s = style.selection;
    const float radius = style.simple ? std::min(kSimpleRowRadius * scale, (capsule.bottom - capsule.top) / 2.0f)
                                      : (capsule.bottom - capsule.top) / 2.0f;

    auto fill = SolidBrush(target, s.fill);
    target->FillRoundedRectangle(Rounded(capsule, radius), fill.get());
    if (style.simple) {
        const float inset = std::round(8.0f * scale);
        const float rail = std::max(1.0f, std::round(kSimpleRailWidth * scale));
        const float vertical = std::round(6.0f * scale);
        auto accent = SolidBrush(target, style.rail);
        target->FillRoundedRectangle(
            Rounded(D2D1::RectF(capsule.left + inset, capsule.top + vertical, capsule.left + inset + rail,
                                capsule.bottom - vertical),
                    rail / 2.0f),
            accent.get());
        return;
    }
    if (!s.glass) return;

    const auto top = D2D1::Point2F(0.0f, capsule.top);
    const auto bottom = D2D1::Point2F(0.0f, capsule.bottom);
    const D2D1_GRADIENT_STOP recessStops[] = {{0.0f, WithAlpha(s.recess, 1.0f)},
                                              {0.65f, WithAlpha(s.recess, 0.0f)}};
    InnerBand(target, capsule, radius, scale, kRecessDepth, s.recess.a,
              GradientBrush(target, top, bottom, recessStops, 2));
    BottomLip(target, capsule, radius, scale, D2D1::ColorF(1.0f, 1.0f, 1.0f, s.rimBottom));
}
enum class Ink { Normal, Selected };

// Draws the rows' numbers, tags and text. The composition path draws every
// row twice, once per ink, and lets the moving capsule reveal the Selected
// layer; the HWND fallback draws only `onlyRow` in Selected ink.
void DrawRows(ID2D1RenderTarget* target, IDWriteFactory* factory, const TextFormats& formats,
              const Layout& layout, const Frame& frame, const Style& style, Ink ink,
              std::size_t onlyRow, std::size_t skipRow) {
    const bool selected = ink == Ink::Selected;
    const Palette& p = style.palette;
    const SelectionStyle& s = style.selection;
    auto textBrush = SolidBrush(target, selected ? s.text : p.text);
    auto mutedBrush = SolidBrush(target, selected ? s.mutedText : p.mutedText);
    auto tagBrush = SolidBrush(target, selected ? s.tagFill : p.tagFill);
    // The simple style marks the selection with its row and rail only.
    IDWriteTextFormat* textFormat =
        selected && !style.simple ? formats.textSelected.get() : formats.text.get();

    for (std::size_t i = 0; i < frame.rows.size(); ++i) {
        if ((onlyRow != kNoRow && i != onlyRow) || i == skipRow) continue;
        const Row& row = frame.rows[i];
        const D2D1_RECT_F rect = layout.RowRect(i);

        const float numberLeft = rect.left + layout.rowPaddingLeft;
        const D2D1_RECT_F numberRect =
            D2D1::RectF(numberLeft, rect.top, numberLeft + layout.numberWidth, rect.bottom);
        if (formats.number) {
            target->DrawText(row.number.c_str(), static_cast<UINT32>(row.number.size()),
                             formats.number.get(), numberRect, mutedBrush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        D2D1_RECT_F textRect = D2D1::RectF(numberRect.right + layout.numberGap, rect.top,
                                           rect.right - layout.rowPaddingRight, rect.bottom);
        if (!row.label.empty()) {
            if (auto tag = TagLayout(factory, formats.tag.get(), row.label, layout.tagHeight,
                                     layout.tagTracking)) {
                const float tagWidth = LayoutWidth(tag.get()) + 2.0f * layout.tagPaddingX;
                const float tagTop = std::round(rect.top + (layout.rowHeight - layout.tagHeight) / 2.0f);
                const D2D1_RECT_F tagRect = D2D1::RectF(textRect.right - tagWidth, tagTop,
                                                        textRect.right, tagTop + layout.tagHeight);
                target->FillRoundedRectangle(Rounded(tagRect, layout.tagHeight / 2.0f), tagBrush.get());
                target->DrawTextLayout(D2D1::Point2F(tagRect.left + layout.tagPaddingX, tagRect.top),
                                       tag.get(), mutedBrush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                textRect.right = tagRect.left - layout.tagGap;
            }
        }
        if (textFormat) {
            target->DrawText(row.text.c_str(), static_cast<UINT32>(row.text.size()), textFormat,
                             textRect, textBrush.get(),
                             D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

// Page position: a thin scroll-indicator capsule along the right edge, shown
// only when there are more candidates than fit on the page.
void DrawIndicator(ID2D1RenderTarget* target, const Layout& layout, const Frame& frame,
                   const Style& style) {
    if (!layout.indicator || frame.totalCount <= frame.rows.size()) return;
    const float center = (layout.rowRight + layout.listWidth) / 2.0f;
    const float half = layout.indicatorWidth / 2.0f;
    const float top = layout.indicatorInset;
    const float bottom = layout.listHeight - layout.indicatorInset;
    const float length = bottom - top;
    if (length <= 0.0f) return;

    auto track = SolidBrush(target, style.palette.tagFill);
    target->FillRoundedRectangle(Rounded(D2D1::RectF(center - half, top, center + half, bottom), half),
                                 track.get());

    const float total = static_cast<float>(frame.totalCount);
    const float shown = static_cast<float>(frame.rows.size());
    const float thumbLength = std::max(length * shown / total, layout.indicatorWidth * 4.0f);
    const float travel = std::max(total - shown, 1.0f);
    const float thumbTop =
        top + (length - thumbLength) * std::min(static_cast<float>(frame.firstIndex) / travel, 1.0f);
    auto thumb = SolidBrush(target, style.palette.indicator);
    target->FillRoundedRectangle(
        Rounded(D2D1::RectF(center - half, thumbTop, center + half, thumbTop + thumbLength), half),
        thumb.get());
}

// The meaning pane: a hairline between it and the list, then the headword
// and its senses.
void DrawDetail(ID2D1RenderTarget* target, const Layout& layout, const Style& style) {
    if (!layout.detailText) return;
    auto line = SolidBrush(target, style.palette.tagFill);
    const float x = std::round(layout.listWidth) + 0.5f;
    target->DrawLine(D2D1::Point2F(x, layout.padding * 2.0f),
                     D2D1::Point2F(x, static_cast<float>(layout.height) - layout.padding * 2.0f), line.get(),
                     std::max(1.0f, layout.scale));
    auto text = SolidBrush(target, style.palette.text);
    target->DrawTextLayout(D2D1::Point2F(layout.detail.left, layout.detail.top), layout.detailText.get(),
                           text.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Signed distance from (x, y) to a rounded rectangle; negative inside.
float RoundedRectDistance(float x, float y, float centerX, float centerY, float halfWidth,
                          float halfHeight, float radius) {
    const float qx = std::fabs(x - centerX) - (halfWidth - radius);
    const float qy = std::fabs(y - centerY) - (halfHeight - radius);
    const float ox = std::max(qx, 0.0f);
    const float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

struct ShadowLayer {
    float sigma;
    float offset;
    float alpha;
};

// Rasterizes the panel's drop shadow as premultiplied black, with the panel
// itself cut out (antialiased) so the shadow window can sit above the glass
// without darkening it. A Gaussian-blurred edge is approximated by erfc of
// the signed distance, which is exact for straight edges and close enough
// around corners whose radius exceeds the blur.
void RasterizeShadow(std::uint32_t* pixels, int width, int height, float panelX, float panelY,
                     float panelWidth, float panelHeight, float radius, ShadowLayer key,
                     ShadowLayer ambient) {
    const float halfWidth = panelWidth / 2.0f;
    const float halfHeight = panelHeight / 2.0f;
    const float centerX = panelX + halfWidth;
    const float centerY = panelY + halfHeight;
    constexpr float kInvSqrt2 = 0.70710678f;
    const auto blurred = [&](float x, float y, const ShadowLayer& layer) {
        if (layer.alpha <= 0.0f || layer.sigma <= 0.0f) return 0.0f;
        const float distance =
            RoundedRectDistance(x, y - layer.offset, centerX, centerY, halfWidth, halfHeight, radius);
        return layer.alpha * 0.5f * std::erfc(distance * kInvSqrt2 / layer.sigma);
    };
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const float x = static_cast<float>(column) + 0.5f;
            const float y = static_cast<float>(row) + 0.5f;
            const float inside =
                std::clamp(0.5f - RoundedRectDistance(x, y, centerX, centerY, halfWidth, halfHeight, radius),
                           0.0f, 1.0f);
            float alpha = 0.0f;
            if (inside < 1.0f) {
                const float a1 = blurred(x, y, key);
                const float a2 = blurred(x, y, ambient);
                alpha = (1.0f - (1.0f - a1) * (1.0f - a2)) * (1.0f - inside);
            }
            pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(column)] =
                static_cast<std::uint32_t>(std::lround(alpha * 255.0f)) << 24;
        }
    }
}

// ---------------------------------------------------------------------------
// UI thread side.

struct Surface {
    wuc::CompositionDrawingSurface surface{nullptr};
    SIZE size{0, 0};
};

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET;
}

// CLSID_D2D1Saturation, spelled out so the DLL needs no dxguid.lib.
constexpr GUID kSaturationEffectId{0x5cb2d9cf, 0x327d, 0x459f, {0xa0, 0xce, 0x40, 0xc0, 0xb2, 0x08, 0x6b, 0xf7}};

// Windows.Graphics.Effects description of the Direct2D saturation effect,
// the minimum the compositor needs to build an effect brush (what Win2D
// would otherwise provide).
struct SaturationEffect
    : winrt::implements<SaturationEffect, winrt::Windows::Graphics::Effects::IGraphicsEffect,
                        winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
                        ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> {
    SaturationEffect(winrt::Windows::Graphics::Effects::IGraphicsEffectSource source, float saturation)
        : source_(std::move(source)), saturation_(saturation) {}

    winrt::hstring Name() const { return name_; }
    void Name(const winrt::hstring& name) { name_ = name; }

    HRESULT __stdcall GetEffectId(GUID* id) noexcept override {
        *id = kSaturationEffectId;
        return S_OK;
    }

    HRESULT __stdcall GetNamedPropertyMapping(
        LPCWSTR name, UINT* index,
        ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override {
        if (!name || wcscmp(name, L"Saturation") != 0) return E_INVALIDARG;
        *index = D2D1_SATURATION_PROP_SATURATION;
        *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }

    HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override {
        *count = 1;
        return S_OK;
    }

    HRESULT __stdcall GetProperty(UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept override {
        if (index != D2D1_SATURATION_PROP_SATURATION) return E_INVALIDARG;
        try {
            *value = winrt::Windows::Foundation::PropertyValue::CreateSingle(saturation_)
                         .as<ABI::Windows::Foundation::IPropertyValue>()
                         .detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT __stdcall GetSource(UINT index,
                                ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept override {
        if (index != 0) return E_INVALIDARG;
        try {
            *source = source_.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().detach();
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT __stdcall GetSourceCount(UINT* count) noexcept override {
        *count = 1;
        return S_OK;
    }

private:
    winrt::Windows::Graphics::Effects::IGraphicsEffectSource source_;
    float saturation_;
    winrt::hstring name_;
};

class CandidateView final {
public:
    explicit CandidateView(std::shared_ptr<CandidateWindow::Channel> channel)
        : channel_(std::move(channel)) {}
    CandidateView(const CandidateView&) = delete;
    CandidateView& operator=(const CandidateView&) = delete;

    bool Start() {
        // Isolated: the private font loader below must not land in the
        // host application's shared DirectWrite factory.
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite_.put()));
        brandFont_ = LoadBrandFont(dwrite_.get(), channel_->instance);
        if (!RegisterWindowClass(kControlClassName) || !RegisterWindowClass(kWindowClassName) ||
            !RegisterWindowClass(kShadowClassName)) {
            return false;
        }
        control_ = CreateWindowExW(0, kControlClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   channel_->instance, this);
        return control_ != nullptr && dwrite_ != nullptr;
    }

    [[nodiscard]] HWND Control() const noexcept { return control_; }

    void Shutdown() noexcept {
        if (shadow_) DestroyWindow(shadow_);
        if (panel_) DestroyWindow(panel_);
        shadow_ = panel_ = nullptr;
        ReleaseComposition();
        ReleaseShadowBitmap();
        hwndTarget_ = nullptr;
        formats_ = {};
        brandFont_.collection = nullptr;
        if (brandFont_.loader && dwrite_) {
            if (const auto factory5 = dwrite_.try_as<IDWriteFactory5>()) {
                factory5->UnregisterFontFileLoader(brandFont_.loader.get());
            }
        }
        brandFont_.loader = nullptr;
        ShutdownDispatcherQueue();
        for (const auto* name : {kShadowClassName, kWindowClassName, kControlClassName}) {
            UnregisterClassW(name, channel_->instance);
        }
    }

private:
    bool RegisterWindowClass(const wchar_t* name) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = channel_->instance;
        wc.lpfnWndProc = &CandidateView::WindowProc;
        wc.lpszClassName = name;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<CandidateView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<CandidateView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message) {
        case WM_MOUSEACTIVATE:
            // Never take focus from the application being typed into.
            return MA_NOACTIVATE;
        case kMsgUpdate:
            self->OnUpdate();
            return 0;
        case kMsgQuit:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (hwnd == self->control_) PostQuitMessage(0);
            break;
        case WM_LBUTTONDOWN:
            if (hwnd == self->panel_) {
                self->OnClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            break;
        case WM_PAINT:
            if (hwnd == self->panel_ && !self->composition_) {
                self->PaintFallback();
                return 0;
            }
            break;
        case WM_DPICHANGED:
            // Size and position are recomputed from the caret on every update.
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void OnUpdate() {
        Frame frame;
        {
            std::lock_guard<std::mutex> lock(channel_->mutex);
            if (!channel_->hasPending) return;
            frame = std::move(channel_->pending);
            channel_->hasPending = false;
        }
        try {
            if (!frame.visible) {
                HidePopup();
                return;
            }
            Present(std::move(frame));
        } catch (...) {
            // A composition failure must never take the host down; drop to
            // the HWND renderer and try again once.
            if (composition_) {
                SwitchToFallback();
                try {
                    Present(Frame(frame_));
                } catch (...) {
                    HidePopup();
                }
            } else {
                HidePopup();
            }
        }
    }

    void OnClick(int x, int y) {
        if (!visible_ || layout_.rowPitch <= 0.0f) return;
        const float fy = static_cast<float>(y) - layout_.padding;
        const float fx = static_cast<float>(x);
        if (fy < 0.0f || fx < layout_.padding || fx > layout_.rowRight) return;
        // Rows are separated by a gap, so the hit test uses the same pitch
        // the rows are drawn with and ignores clicks between rows.
        const auto row = static_cast<std::size_t>(fy / layout_.rowPitch);
        if (row >= frame_.rows.size()) return;
        if (fy - static_cast<float>(row) * layout_.rowPitch > layout_.rowHeight) return;
        PostMessageW(channel_->notifyHwnd, kMsgSelect, static_cast<WPARAM>(row),
                     static_cast<LPARAM>(frame_.generation));
    }

    void Present(Frame frame) {
        if (!EnsureWindows()) return;
        if (!visible_ || frame.style != frame_.style) {
            style_ = ReadStyle(composition_ && hostBackdrop_, frame.style == 1);
        }

        // DPI follows the monitor under the caret; move there first so
        // GetDpiForWindow reports the DPI the panel is about to render at.
        if (MonitorFromRect(&frame.caret, MONITOR_DEFAULTTONEAREST) !=
            MonitorFromWindow(panel_, MONITOR_DEFAULTTONEAREST)) {
            SetWindowPos(panel_, nullptr, frame.caret.left, frame.caret.bottom, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        EnsureTextFormats();

        const Layout layout = ComputeLayout(frame);
        const POINT position = CandidatePosition(frame.caret, layout.width, layout.height,
                                                 ScaleForWindow(panel_, kCaretGap));
        const bool animate = style_.animations && visible_ && frame_.selectedRow < frame_.rows.size() &&
                             frame.selectedRow < frame.rows.size() &&
                             frame.firstIndex == frame_.firstIndex &&
                             frame.selectedRow != frame_.selectedRow;
        frame_ = std::move(frame);
        layout_ = layout;

        if (composition_) {
            HRESULT hr = RenderComposition(animate);
            if (IsDeviceLost(hr) && RecreateDevices()) hr = RenderComposition(false);
            winrt::check_hresult(hr);
            UpdateShadow(position);
            HDWP batch = BeginDeferWindowPos(2);
            if (batch && shadow_) {
                batch = DeferWindowPos(batch, shadow_, HWND_TOPMOST, 0, 0, 0, 0,
                                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            if (batch) {
                // Directly below the (click-through) shadow window.
                batch = DeferWindowPos(batch, panel_, shadow_ ? shadow_ : HWND_TOPMOST, position.x,
                                       position.y, layout.width, layout.height,
                                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            if (batch) EndDeferWindowPos(batch);
        } else {
            SetWindowPos(panel_, HWND_TOPMOST, position.x, position.y, layout.width, layout.height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (hwndTarget_) {
                hwndTarget_->Resize(D2D1::SizeU(static_cast<UINT32>(layout.width),
                                                static_cast<UINT32>(layout.height)));
            }
            InvalidateRect(panel_, nullptr, FALSE);
        }
        visible_ = true;
    }

    void HidePopup() noexcept {
        if (!visible_) return;
        visible_ = false;
        try {
            // Hidden with zero opacity, so a stale tree never flashes for a
            // frame when the window is shown again before the next commit.
            if (root_) root_.Opacity(0.0f);
        } catch (...) {
        }
        if (shadow_) ShowWindow(shadow_, SW_HIDE);
        if (panel_) ShowWindow(panel_, SW_HIDE);
    }

    Layout ComputeLayout(const Frame& frame) const {
        Layout l;
        l.scale = ScaleFactor(panel_);
        l.radius = static_cast<float>(
            ScaleForWindow(panel_, !composition_                ? kFallbackCornerRadius
                                   : frame.style == 1 ? kSimpleCornerRadius
                                                      : kCornerRadius));
        l.padding = static_cast<float>(ScaleForWindow(panel_, kPanelPadding));
        l.rowHeight = static_cast<float>(ScaleForWindow(panel_, kRowHeight));
        l.rowPitch = l.rowHeight + static_cast<float>(ScaleForWindow(panel_, kRowGap));
        l.rowPaddingLeft = static_cast<float>(ScaleForWindow(panel_, kRowPaddingLeft));
        l.numberWidth = static_cast<float>(ScaleForWindow(panel_, kNumberWidth));
        l.numberGap = static_cast<float>(ScaleForWindow(panel_, kNumberGap));
        l.rowPaddingRight = static_cast<float>(ScaleForWindow(panel_, kRowPaddingRight));
        l.tagGap = static_cast<float>(ScaleForWindow(panel_, kTagGap));
        l.tagPaddingX = static_cast<float>(ScaleForWindow(panel_, kTagPaddingX));
        l.tagHeight = static_cast<float>(ScaleForWindow(panel_, kTagHeight));
        l.tagTracking = kTagTracking * l.scale;
        l.indicator = frame.totalCount > frame.rows.size();
        l.indicatorWidth = static_cast<float>(ScaleForWindow(panel_, kIndicatorWidth));
        l.indicatorInset = static_cast<float>(ScaleForWindow(panel_, kIndicatorInset));

        // Candidate length varies a lot (a single emoji vs. a long phrase), so
        // the panel is sized to the widest visible row. Measured in the
        // selected weight so moving the selection never changes the width.
        float widest = 0.0f;
        for (const Row& row : frame.rows) {
            float content = l.rowPaddingLeft + l.numberWidth + l.numberGap +
                            MeasureTextWidth(dwrite_.get(), formats_.textSelected.get(), row.text) +
                            l.rowPaddingRight;
            if (!row.label.empty()) {
                const auto tag = TagLayout(dwrite_.get(), formats_.tag.get(), row.label, l.tagHeight,
                                           l.tagTracking);
                content += l.tagGap + LayoutWidth(tag.get()) + 2.0f * l.tagPaddingX;
            }
            widest = std::max(widest, content);
        }
        const int gutter = l.indicator ? ScaleForWindow(panel_, kIndicatorGutter) : 0;
        l.width = std::clamp(static_cast<int>(std::ceil(widest + 2.0f * l.padding)) + 1 + gutter,
                             ScaleForWindow(panel_, kMinWidth), ScaleForWindow(panel_, kMaxWidth));
        const auto count = static_cast<float>(std::max<std::size_t>(frame.rows.size(), 1));
        l.height = static_cast<int>(
            std::lround(2.0f * l.padding + count * l.rowHeight + (count - 1.0f) * (l.rowPitch - l.rowHeight)));
        l.rowRight = static_cast<float>(l.width) - l.padding - static_cast<float>(gutter);
        l.listWidth = static_cast<float>(l.width);
        l.listHeight = static_cast<float>(l.height);
        AddDetail(l, frame);
        return l;
    }

    // Widens the layout by the meaning pane when the frame has a meaning.
    void AddDetail(Layout& l, const Frame& frame) const {
        if (frame.detailSenses.empty() || !formats_.detail || !dwrite_) return;
        const float pad = static_cast<float>(ScaleForWindow(panel_, kDetailPadding));
        const float inner = static_cast<float>(ScaleForWindow(panel_, kDetailWidth)) - 2.0f * pad;
        const float maxText = static_cast<float>(ScaleForWindow(panel_, kDetailMaxHeight)) - 2.0f * pad;
        static constexpr wchar_t kMarks[] = L"\u2460\u2461\u2462\u2463\u2464\u2465\u2466\u2467\u2468";  // circled 1 to 9
        winrt::com_ptr<IDWriteTextLayout> text;
        float textHeight = 0.0f;
        // Later senses give way until the text fits.
        for (std::size_t count = frame.detailSenses.size(); count > 0; --count) {
            std::wstring content = frame.detailHead;
            for (std::size_t i = 0; i < count; ++i) {
                content += L'\n';
                if (frame.detailSenses.size() > 1 && i < 9) {
                    content += kMarks[i];
                    content += L' ';
                }
                content += frame.detailSenses[i];
            }
            text = nullptr;
            if (FAILED(dwrite_->CreateTextLayout(content.c_str(), static_cast<UINT32>(content.size()),
                                                 formats_.detail.get(), inner, maxText, text.put()))) {
                return;
            }
            const DWRITE_TEXT_RANGE head{0, static_cast<UINT32>(frame.detailHead.size())};
            text->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, head);
            text->SetFontSize(kFontDetailHead * l.scale, head);
            DWRITE_TEXT_METRICS metrics{};
            text->GetMetrics(&metrics);
            textHeight = metrics.height;
            if (textHeight <= maxText) break;
        }
        if (!text) return;
        const int width = ScaleForWindow(panel_, kDetailWidth);
        l.width += width;
        l.height = std::max(l.height, static_cast<int>(std::ceil(std::min(textHeight, maxText) + 2.0f * pad)));
        l.detail = D2D1::RectF(l.listWidth + pad, pad, static_cast<float>(l.width) - pad,
                               static_cast<float>(l.height) - pad);
        l.detailText = std::move(text);
    }

    void EnsureTextFormats() {
        const UINT dpi = GetDpiForWindow(panel_);
        if (formats_.dpi == dpi && formats_.text) return;
        const float scale = static_cast<float>(dpi == 0 ? kBaseDpi : dpi) / static_cast<float>(kBaseDpi);
        IDWriteFactory* f = dwrite_.get();
        IDWriteFontCollection* brand = brandFont_.collection.get();
        formats_.dpi = dpi;
        formats_.text = CreateFormat(f, brand, kFontCandidate * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
        formats_.textSelected = CreateFormat(f, brand, kFontCandidate * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_TEXT_ALIGNMENT_LEADING, true);
        formats_.number = CreateFormat(f, brand, kFontNumber * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                       DWRITE_TEXT_ALIGNMENT_TRAILING, false);
        formats_.tag = CreateFormat(f, brand, kFontTag * scale, DWRITE_FONT_WEIGHT_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_LEADING, false);
        formats_.detail = CreateFormat(f, brand, kFontDetail * scale, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_TEXT_ALIGNMENT_LEADING, false);
        if (formats_.detail) {
            formats_.detail->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            formats_.detail->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }

    // --- windows -----------------------------------------------------------

    bool EnsureWindows() {
        if (panel_) return true;
        if (windowsFailed_) return false;

        if (!compositionBroken_ && InitComposition()) {
            panel_ = CreatePanel(WS_EX_NOREDIRECTIONBITMAP);
            if (panel_) {
                BOOL enable = TRUE;
                hostBackdrop_ = SUCCEEDED(DwmSetWindowAttribute(panel_, DWMWA_USE_HOSTBACKDROPBRUSH,
                                                                &enable, sizeof(enable)));
                const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(panel_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
                const COLORREF noBorder = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(panel_, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
                composition_ = BuildVisualTree();
                if (composition_) {
                    shadow_ = CreateWindowExW(
                        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                            WS_EX_TOPMOST,
                        kShadowClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                        channel_->instance, this);
                    return true;
                }
                DestroyWindow(panel_);
                panel_ = nullptr;
            }
            ReleaseComposition();
            compositionBroken_ = true;
        }

        composition_ = false;
        panel_ = CreatePanel(0);
        if (!panel_) {
            windowsFailed_ = true;
            return false;
        }
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(panel_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        // Extending the frame gives the popup a DWM shadow.
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(panel_, &margins);
        return true;
    }

    HWND CreatePanel(DWORD extraStyle) {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | extraStyle,
                               kWindowClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               channel_->instance, this);
    }

    void SwitchToFallback() noexcept {
        if (shadow_) DestroyWindow(shadow_);
        if (panel_) DestroyWindow(panel_);
        shadow_ = panel_ = nullptr;
        visible_ = false;
        ReleaseComposition();
        compositionBroken_ = true;
        composition_ = false;
    }

    // --- composition ---------------------------------------------------------

    bool InitComposition() {
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
        if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_NO_COMPOSITION", nullptr, 0) > 0) return false;
#endif
        try {
            if (!dispatcherQueue_) {
                DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
                winrt::check_hresult(CreateDispatcherQueueController(
                    options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                                 winrt::put_abi(dispatcherQueue_))));
            }
            compositor_ = wuc::Compositor();
            if (!CreateDevices()) {
                ReleaseComposition();
                return false;
            }
            winrt::check_hresult(compositor_.as<abi::ICompositorInterop>()->CreateGraphicsDevice(
                d2dDevice_.get(),
                reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(graphics_))));
            return true;
        } catch (...) {
            ReleaseComposition();
            return false;
        }
    }

    bool CreateDevices() {
        d2dDevice_ = nullptr;
        d3dDevice_ = nullptr;
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                       D3D11_SDK_VERSION, d3dDevice_.put(), nullptr, nullptr);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, d3dDevice_.put(), nullptr, nullptr);
        }
        if (FAILED(hr)) return false;
        if (!d2dFactory_) {
            // Multithreaded: the compositor may touch the device off this thread.
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                                         nullptr, d2dFactory_.put_void()))) {
                return false;
            }
        }
        const auto dxgi = d3dDevice_.try_as<IDXGIDevice>();
        return dxgi && SUCCEEDED(d2dFactory_->CreateDevice(dxgi.get(), d2dDevice_.put()));
    }

    bool RecreateDevices() {
        try {
            if (!CreateDevices()) return false;
            winrt::check_hresult(graphics_.as<abi::ICompositionGraphicsDeviceInterop>()->SetRenderingDevice(
                d2dDevice_.get()));
            return true;
        } catch (...) {
            return false;
        }
    }

    wuc::SpriteVisual SurfaceVisual(Surface& surface) {
        surface.surface = graphics_.CreateDrawingSurface(
            {1.0f, 1.0f}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        surface.size = {1, 1};
        auto brush = compositor_.CreateSurfaceBrush(surface.surface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0.0f);
        brush.VerticalAlignmentRatio(0.0f);
        auto visual = compositor_.CreateSpriteVisual();
        visual.Brush(brush);
        return visual;
    }

    // root
    //  +- glass       backdrop blur (or solid color), clipped to the panel shape
    //  +- chrome      veil, inner glow, specular rim
    //  +- content     rows in normal ink, page indicator
    //  +- selection   moves between rows (implicitly animated)
    //      +- capsule    tinted glass pill with its lift shadow
    //      +- window     clipped to the pill
    //          +- selectedText   rows in selected ink, held still in panel space
    bool BuildVisualTree() {
        try {
            winrt::check_hresult(
                compositor_.as<abi::Desktop::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
                    panel_, TRUE,
                    reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target_))));
            root_ = compositor_.CreateContainerVisual();
            target_.Root(root_);

            glassVisual_ = compositor_.CreateSpriteVisual();
            glassClip_ = compositor_.CreateRectangleClip();
            glassVisual_.Clip(glassClip_);
            solidBrush_ = compositor_.CreateColorBrush();
            if (hostBackdrop_) backdropBrush_ = VibrantBackdrop();

            chromeVisual_ = SurfaceVisual(chrome_);
            contentVisual_ = SurfaceVisual(content_);
            selectionVisual_ = compositor_.CreateContainerVisual();
            capsuleVisual_ = SurfaceVisual(capsule_);
            textWindow_ = compositor_.CreateContainerVisual();
            textClip_ = compositor_.CreateRectangleClip();
            textWindow_.Clip(textClip_);
            selectedTextVisual_ = SurfaceVisual(selectedText_);

            // The light lives inside the glass visual so the panel clip
            // applies, and its anchor tracks the selection's (animated) offset.
            lightAnchor_ = compositor_.CreateContainerVisual();
            lightVisual_ = SurfaceVisual(light_);
            lightAnchor_.Children().InsertAtTop(lightVisual_);
            glassVisual_.Children().InsertAtTop(lightAnchor_);
            root_.Children().InsertAtTop(glassVisual_);
            root_.Children().InsertAtTop(chromeVisual_);
            root_.Children().InsertAtTop(contentVisual_);
            root_.Children().InsertAtTop(selectionVisual_);
            selectionVisual_.Children().InsertAtTop(capsuleVisual_);
            selectionVisual_.Children().InsertAtTop(textWindow_);
            textWindow_.Children().InsertAtTop(selectedTextVisual_);

            // The selected-ink text stays fixed in panel space while the
            // capsule moves, so the capsule reveals it like a lens sliding
            // over the list instead of the text jumping row to row.
            auto counter = compositor_.CreateExpressionAnimation(
                L"Vector3(-selection.Offset.X, -selection.Offset.Y, 0)");
            counter.SetReferenceParameter(L"selection", selectionVisual_);
            selectedTextVisual_.StartAnimation(L"Offset", counter);
            auto follow = compositor_.CreateExpressionAnimation(L"selection.Offset");
            follow.SetReferenceParameter(L"selection", selectionVisual_);
            lightAnchor_.StartAnimation(L"Offset", follow);

            // A quick glide with a hint of overshoot.
            auto easing = compositor_.CreateCubicBezierEasingFunction(float2{0.25f, 1.1f},
                                                                      float2{0.4f, 1.0f});
            auto glide = compositor_.CreateVector3KeyFrameAnimation();
            glide.Target(L"Offset");
            glide.InsertExpressionKeyFrame(1.0f, L"this.FinalValue", easing);
            glide.Duration(kGlideDuration);
            selectionAnimations_ = compositor_.CreateImplicitAnimationCollection();
            selectionAnimations_.Insert(L"Offset", glide);
            selectionVisual_.ImplicitAnimations(selectionAnimations_);
            return true;
        } catch (...) {
            return false;
        }
    }

    // The blurred desktop behind the window, with its color boosted. Falls
    // back to the plain backdrop if the effect cannot be compiled.
    wuc::CompositionBrush VibrantBackdrop() {
        auto backdrop = compositor_.CreateHostBackdropBrush();
        try {
            const auto effect = winrt::make<SaturationEffect>(
                wuc::CompositionEffectSourceParameter(L"backdrop"), kBackdropSaturation);
            auto brush = compositor_.CreateEffectFactory(effect).CreateBrush();
            brush.SetSourceParameter(L"backdrop", backdrop);
            return brush;
        } catch (...) {
            return backdrop;
        }
    }

    void ReleaseComposition() noexcept {
        selectionAnimations_ = nullptr;
        lightVisual_ = nullptr;
        lightAnchor_ = nullptr;
        light_ = {};
        selectedTextVisual_ = nullptr;
        textClip_ = nullptr;
        textWindow_ = nullptr;
        capsuleVisual_ = nullptr;
        selectionVisual_ = nullptr;
        contentVisual_ = nullptr;
        chromeVisual_ = nullptr;
        backdropBrush_ = nullptr;
        solidBrush_ = nullptr;
        glassClip_ = nullptr;
        glassVisual_ = nullptr;
        root_ = nullptr;
        target_ = nullptr;
        chrome_ = {};
        content_ = {};
        capsule_ = {};
        selectedText_ = {};
        graphics_ = nullptr;
        compositor_ = nullptr;
        d2dDevice_ = nullptr;
        d3dDevice_ = nullptr;
    }

    void ShutdownDispatcherQueue() noexcept {
        if (!dispatcherQueue_) return;
        try {
            const auto operation = dispatcherQueue_.ShutdownQueueAsync();
            const auto deadline = GetTickCount64() + 1000;
            while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started &&
                   GetTickCount64() < deadline) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } catch (...) {
        }
        dispatcherQueue_ = nullptr;
    }

    template <typename Draw>
    HRESULT PaintSurface(Surface& surface, int width, int height, Draw&& draw) {
        width = std::max(width, 1);
        height = std::max(height, 1);
        const auto interop = surface.surface.as<abi::ICompositionDrawingSurfaceInterop>();
        if (surface.size.cx != width || surface.size.cy != height) {
            const HRESULT hr = interop->Resize({width, height});
            if (FAILED(hr)) return hr;
            surface.size = {width, height};
        }
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        HRESULT hr = interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
        if (FAILED(hr)) return hr;
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                       static_cast<float>(offset.y)));
        dc->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                                            static_cast<float>(height)),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        draw(dc.get());
        dc->PopAxisAlignedClip();
        return interop->EndDraw();
    }

    static winrt::Windows::UI::Color ToUiColor(const D2D1_COLOR_F& color) {
        const auto byte = [](float channel) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
        };
        return {byte(color.a), byte(color.r), byte(color.g), byte(color.b)};
    }

    HRESULT RenderComposition(bool animate) {
        const float width = static_cast<float>(layout_.width);
        const float height = static_cast<float>(layout_.height);
        const float2 radius{layout_.radius, layout_.radius};

        root_.Opacity(1.0f);
        glassVisual_.Size({width, height});
        glassClip_.Right(width);
        glassClip_.Bottom(height);
        glassClip_.TopLeftRadius(radius);
        glassClip_.TopRightRadius(radius);
        glassClip_.BottomRightRadius(radius);
        glassClip_.BottomLeftRadius(radius);
        solidBrush_.Color(ToUiColor(style_.palette.solid));
        if (style_.glass && backdropBrush_) {
            glassVisual_.Brush(backdropBrush_);
        } else {
            glassVisual_.Brush(solidBrush_);
        }

        chromeVisual_.Size({width, height});
        HRESULT hr = PaintSurface(chrome_, layout_.width, layout_.height,
                                  [&](ID2D1RenderTarget* dc) { DrawChrome(dc, layout_, style_); });
        if (FAILED(hr)) return hr;

        contentVisual_.Size({width, height});
        hr = PaintSurface(content_, layout_.width, layout_.height, [&](ID2D1RenderTarget* dc) {
            DrawRows(dc, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Normal, kNoRow, kNoRow);
            DrawIndicator(dc, layout_, frame_, style_);
            DrawDetail(dc, layout_, style_);
        });
        if (FAILED(hr)) return hr;

        if (frame_.selectedRow >= frame_.rows.size()) {
            selectionVisual_.IsVisible(false);
            lightAnchor_.IsVisible(false);
            return S_OK;
        }

        const D2D1_RECT_F row = layout_.RowRect(frame_.selectedRow);
        const float rowWidth = row.right - row.left;
        const float rowHeight = row.bottom - row.top;
        const float pad = static_cast<float>(ScaleForWindow(panel_, kSelectionPad));
        const int capsuleWidth = static_cast<int>(std::ceil(rowWidth + 2.0f * pad));
        const int capsuleHeight = static_cast<int>(std::ceil(rowHeight + 2.0f * pad));
        const float spread = static_cast<float>(ScaleForWindow(panel_, kLightSpread) + 2);
        const int lightWidth = static_cast<int>(std::ceil(rowWidth + 2.0f * spread));
        const int lightHeight = static_cast<int>(std::ceil(rowHeight + 2.0f * spread));
        lightVisual_.Offset({-spread, -spread, 0.0f});
        lightVisual_.Size({static_cast<float>(lightWidth), static_cast<float>(lightHeight)});
        hr = PaintSurface(light_, lightWidth, lightHeight, [&](ID2D1RenderTarget* dc) {
            DrawLight(dc, D2D1::RectF(spread, spread, spread + rowWidth, spread + rowHeight), layout_.scale,
                      style_);
        });
        if (FAILED(hr)) return hr;
        lightAnchor_.IsVisible(true);

        capsuleVisual_.Offset({-pad, -pad, 0.0f});
        capsuleVisual_.Size({static_cast<float>(capsuleWidth), static_cast<float>(capsuleHeight)});
        hr = PaintSurface(capsule_, capsuleWidth, capsuleHeight, [&](ID2D1RenderTarget* dc) {
            DrawSelection(dc, D2D1::RectF(pad, pad, pad + rowWidth, pad + rowHeight), layout_.scale,
                          style_);
        });
        if (FAILED(hr)) return hr;

        const float corner = style_.simple
                                 ? std::min(static_cast<float>(ScaleForWindow(panel_, kSimpleRowRadius)), rowHeight / 2.0f)
                                 : rowHeight / 2.0f;
        const float2 pill{corner, corner};
        textWindow_.Size({rowWidth, rowHeight});
        textClip_.Right(rowWidth);
        textClip_.Bottom(rowHeight);
        textClip_.TopLeftRadius(pill);
        textClip_.TopRightRadius(pill);
        textClip_.BottomRightRadius(pill);
        textClip_.BottomLeftRadius(pill);
        selectedTextVisual_.Size({width, height});
        hr = PaintSurface(selectedText_, layout_.width, layout_.height, [&](ID2D1RenderTarget* dc) {
            DrawRows(dc, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Selected, kNoRow, kNoRow);
        });
        if (FAILED(hr)) return hr;

        const float3 offset{row.left, row.top, 0.0f};
        if (animate) {
            selectionVisual_.Offset(offset);
        } else {
            selectionVisual_.ImplicitAnimations(nullptr);
            selectionVisual_.Offset(offset);
            selectionVisual_.ImplicitAnimations(selectionAnimations_);
        }
        selectionVisual_.IsVisible(true);
        return S_OK;
    }

    // --- shadow window -------------------------------------------------------

    void ReleaseShadowBitmap() noexcept {
        if (shadowDc_) {
            if (shadowOldBitmap_) SelectObject(shadowDc_, shadowOldBitmap_);
            DeleteDC(shadowDc_);
        }
        if (shadowBitmap_) DeleteObject(shadowBitmap_);
        shadowDc_ = nullptr;
        shadowBitmap_ = nullptr;
        shadowOldBitmap_ = nullptr;
        shadowBits_ = nullptr;
        shadowSize_ = {0, 0};
        shadowKey_ = {};
    }

    void UpdateShadow(POINT panelPosition) {
        if (!shadow_) return;
        const int marginX = ScaleForWindow(panel_, kShadowMarginX);
        const int marginTop = ScaleForWindow(panel_, kShadowMarginTop);
        const int marginBottom = ScaleForWindow(panel_, kShadowMarginBottom);
        const SIZE size{layout_.width + 2 * marginX, layout_.height + marginTop + marginBottom};
        const std::array<float, 5> key{static_cast<float>(layout_.width),
                                       static_cast<float>(layout_.height), layout_.radius,
                                       style_.palette.shadowKey, style_.palette.shadowAmbient};

        if (key != shadowKey_ || !shadowBits_) {
            if (size.cx != shadowSize_.cx || size.cy != shadowSize_.cy || !shadowBits_) {
                ReleaseShadowBitmap();
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(info.bmiHeader);
                info.bmiHeader.biWidth = size.cx;
                info.bmiHeader.biHeight = -size.cy;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                shadowDc_ = CreateCompatibleDC(nullptr);
                shadowBitmap_ = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &shadowBits_, nullptr, 0);
                if (!shadowDc_ || !shadowBitmap_ || !shadowBits_) {
                    ReleaseShadowBitmap();
                    return;
                }
                shadowOldBitmap_ = SelectObject(shadowDc_, shadowBitmap_);
                shadowSize_ = size;
            }
            const float s = layout_.scale;
            RasterizeShadow(static_cast<std::uint32_t*>(shadowBits_), size.cx, size.cy,
                            static_cast<float>(marginX), static_cast<float>(marginTop),
                            static_cast<float>(layout_.width), static_cast<float>(layout_.height),
                            layout_.radius,
                            {kShadowKeyBlur * s, kShadowKeyOffset * s, style_.palette.shadowKey},
                            {kShadowAmbientBlur * s, kShadowAmbientOffset * s, style_.palette.shadowAmbient});
            shadowKey_ = key;
        }

        POINT destination{panelPosition.x - marginX, panelPosition.y - marginTop};
        SIZE windowSize = shadowSize_;
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(shadow_, nullptr, &destination, &windowSize, shadowDc_, &source, 0, &blend,
                            ULW_ALPHA);
    }

    // --- HWND fallback -------------------------------------------------------

    void PaintFallback() {
        PAINTSTRUCT ps{};
        if (!BeginPaint(panel_, &ps)) return;
        if (!hwndTarget_ && layout_.width > 0) {
            if (!d2dFactory_) {
                D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                  d2dFactory_.put_void());
            }
            if (d2dFactory_) {
                const auto properties = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
                    96.0f);
                d2dFactory_->CreateHwndRenderTarget(
                    properties,
                    D2D1::HwndRenderTargetProperties(
                        panel_, D2D1::SizeU(static_cast<UINT32>(layout_.width),
                                            static_cast<UINT32>(layout_.height))),
                    hwndTarget_.put());
            }
        }
        if (hwndTarget_ && visible_) {
            ID2D1RenderTarget* target = hwndTarget_.get();
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            target->BeginDraw();
            target->Clear(style_.palette.solid);
            if (frame_.selectedRow < frame_.rows.size()) {
                DrawLight(target, layout_.RowRect(frame_.selectedRow), layout_.scale, style_);
            }
            DrawChrome(target, layout_, style_);
            const std::size_t selected = frame_.selectedRow;
            if (selected < frame_.rows.size()) {
                DrawSelection(target, layout_.RowRect(selected), layout_.scale, style_);
            }
            DrawRows(target, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Normal, kNoRow, selected);
            if (selected < frame_.rows.size()) {
                DrawRows(target, dwrite_.get(), formats_, layout_, frame_, style_, Ink::Selected, selected,
                         kNoRow);
            }
            DrawIndicator(target, layout_, frame_, style_);
            DrawDetail(target, layout_, style_);
            if (target->EndDraw() == D2DERR_RECREATE_TARGET) hwndTarget_ = nullptr;
        }
        EndPaint(panel_, &ps);
    }

    std::shared_ptr<CandidateWindow::Channel> channel_;
    HWND control_{nullptr};
    HWND panel_{nullptr};
    HWND shadow_{nullptr};
    bool composition_{false};
    bool compositionBroken_{false};
    bool windowsFailed_{false};
    bool hostBackdrop_{false};
    bool visible_{false};

    Frame frame_;
    Layout layout_;
    Style style_;
    TextFormats formats_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    BrandFont brandFont_;
    winrt::com_ptr<ID2D1Factory1> d2dFactory_;

    winrt::Windows::System::DispatcherQueueController dispatcherQueue_{nullptr};
    wuc::Compositor compositor_{nullptr};
    wuc::Desktop::DesktopWindowTarget target_{nullptr};
    wuc::CompositionGraphicsDevice graphics_{nullptr};
    winrt::com_ptr<ID3D11Device> d3dDevice_;
    winrt::com_ptr<ID2D1Device> d2dDevice_;
    wuc::ContainerVisual root_{nullptr};
    wuc::SpriteVisual glassVisual_{nullptr};
    wuc::ContainerVisual lightAnchor_{nullptr};
    wuc::SpriteVisual lightVisual_{nullptr};
    wuc::RectangleClip glassClip_{nullptr};
    wuc::CompositionColorBrush solidBrush_{nullptr};
    wuc::CompositionBrush backdropBrush_{nullptr};
    wuc::SpriteVisual chromeVisual_{nullptr};
    wuc::SpriteVisual contentVisual_{nullptr};
    wuc::ContainerVisual selectionVisual_{nullptr};
    wuc::SpriteVisual capsuleVisual_{nullptr};
    wuc::ContainerVisual textWindow_{nullptr};
    wuc::RectangleClip textClip_{nullptr};
    wuc::SpriteVisual selectedTextVisual_{nullptr};
    wuc::ImplicitAnimationCollection selectionAnimations_{nullptr};
    Surface light_;
    Surface chrome_;
    Surface content_;
    Surface capsule_;
    Surface selectedText_;

    HDC shadowDc_{nullptr};
    HBITMAP shadowBitmap_{nullptr};
    HGDIOBJ shadowOldBitmap_{nullptr};
    void* shadowBits_{nullptr};
    SIZE shadowSize_{0, 0};
    std::array<float, 5> shadowKey_{};

    winrt::com_ptr<ID2D1HwndRenderTarget> hwndTarget_;
};

struct ThreadStart {
    std::shared_ptr<CandidateWindow::Channel> channel;
    HMODULE module{nullptr};
};

void RunUiThread(const std::shared_ptr<CandidateWindow::Channel>& channel) {
    if (channel->dpiContext) SetThreadDpiAwarenessContext(channel->dpiContext);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        CandidateView view(channel);
        const bool started = view.Start();
        channel->controlHwnd = started ? view.Control() : nullptr;
        SetEvent(channel->ready);
        if (started) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        view.Shutdown();
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
}

DWORD WINAPI UiThreadMain(void* parameter) {
    auto* start = static_cast<ThreadStart*>(parameter);
    const HMODULE module = start->module;
    try {
        RunUiThread(start->channel);
    } catch (...) {
    }
    delete start;
    // The thread holds its own reference on the module, so the DLL cannot be
    // unloaded while this code is still running.
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

CandidateWindow::CandidateWindow() = default;

CandidateWindow::~CandidateWindow() {
    Shutdown();
}

bool CandidateWindow::Initialize(HINSTANCE instance,
                                 std::function<void(std::size_t)> onSelection) {
    onSelection_ = std::move(onSelection);
    if (channel_) {
        return true;
    }
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &CandidateWindow::NotifyWindowProc;
    wc.lpszClassName = kNotifyClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    notifyHwnd_ = CreateWindowExW(0, kNotifyClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  instance, this);
    if (!notifyHwnd_) {
        return false;
    }

    auto channel = std::make_shared<Channel>();
    channel->instance = instance;
    channel->notifyHwnd = notifyHwnd_;
    // Caret rectangles arrive in the host thread's coordinate space, so the
    // UI thread's windows must share its DPI awareness.
    channel->dpiContext = GetThreadDpiAwarenessContext();
    channel->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    HMODULE module = nullptr;
    if (!channel->ready ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&UiThreadMain), &module)) {
        Shutdown();
        return false;
    }
    auto* start = new ThreadStart{channel, module};
    uiThread_ = CreateThread(nullptr, 0, &UiThreadMain, start, 0, nullptr);
    if (!uiThread_) {
        delete start;
        FreeLibrary(module);
        Shutdown();
        return false;
    }
    WaitForSingleObject(channel->ready, 5000);
    channel_ = std::move(channel);
    if (!channel_->controlHwnd) {
        Shutdown();
        return false;
    }
    return true;
}

void CandidateWindow::Show(const RECT& caretRect,
                           const std::vector<Candidate>& candidates,
                           std::size_t selectedIndex,
                           std::size_t pageStart,
                           std::size_t visibleCount,
                           const CandidateDetail& detail) {
    if (!channel_) {
        return;
    }

    if (candidates.empty() || visibleCount == 0) {
        candidates_.clear();
        selectedIndex_ = 0;
        pageStart_ = 0;
        visibleCount_ = 0;
        Hide();
        return;
    }

    candidates_ = candidates;
    selectedIndex_ = selectedIndex == kNoSelection ? kNoSelection
                                                   : std::min(selectedIndex, candidates_.size() - 1);
    pageStart_ = std::min(pageStart, candidates_.size() - 1);
    visibleCount_ = std::min(visibleCount, candidates_.size() - pageStart_);
    if (visibleCount_ == 0) {
        candidates_.clear();
        selectedIndex_ = 0;
        pageStart_ = 0;
        Hide();
        return;
    }

    Frame frame;
    frame.visible = true;
    frame.caret = caretRect;
    frame.generation = ++generation_;
    frame.firstIndex = pageStart_;
    frame.style = style_;
    frame.totalCount = candidates_.size();
    frame.rows.reserve(visibleCount_);
    for (std::size_t row = 0; row < visibleCount_; ++row) {
        const auto index = pageStart_ + row;
        const auto& candidate = candidates_[index];
        const auto displayId = candidate.id == 0 ? index + 1 : candidate.id;
        frame.rows.push_back({std::to_wstring(displayId), candidate.text, LabelText(candidate, japanese_)});
    }
    if (selectedIndex_ >= pageStart_ && selectedIndex_ - pageStart_ < visibleCount_) {
        frame.selectedRow = selectedIndex_ - pageStart_;
    }
    if (!detail.senses.empty()) {
        frame.detailHead = detail.headword;
        frame.detailSenses = detail.senses;
    }

    {
        std::lock_guard<std::mutex> lock(channel_->mutex);
        channel_->pending = std::move(frame);
        channel_->hasPending = true;
    }
    PostMessageW(channel_->controlHwnd, kMsgUpdate, 0, 0);
    shown_ = true;
}

void CandidateWindow::SetStyle(int style) noexcept {
    style_ = style == 1 ? 1 : 0;
}

void CandidateWindow::Hide() noexcept {
    if (!channel_ || !shown_) {
        return;
    }
    shown_ = false;
    ++generation_;  // clicks already in flight refer to the old list
    try {
        std::lock_guard<std::mutex> lock(channel_->mutex);
        channel_->pending = Frame{};
        channel_->pending.generation = generation_;
        channel_->hasPending = true;
    } catch (...) {
        return;
    }
    PostMessageW(channel_->controlHwnd, kMsgUpdate, 0, 0);
}

void CandidateWindow::Shutdown() noexcept {
    if (channel_ && channel_->controlHwnd) {
        PostMessageW(channel_->controlHwnd, kMsgQuit, 0, 0);
    }
    if (uiThread_) {
        // Keep servicing sent messages while waiting so the UI thread can
        // never deadlock against this one; give up after a bounded time (the
        // thread holds its own module reference, so leaving it is safe).
        const auto deadline = GetTickCount64() + 3000;
        for (;;) {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            const DWORD result = MsgWaitForMultipleObjects(
                1, &uiThread_, FALSE, static_cast<DWORD>(deadline - now), QS_SENDMESSAGE);
            if (result != WAIT_OBJECT_0 + 1) break;
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(uiThread_);
        uiThread_ = nullptr;
    }
    if (notifyHwnd_) {
        DestroyWindow(notifyHwnd_);
        notifyHwnd_ = nullptr;
        UnregisterClassW(kNotifyClassName, instance_);
    }
    channel_.reset();
    shown_ = false;
}

LRESULT CALLBACK CandidateWindow::NotifyWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                                   LPARAM lParam) {
    auto* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<CandidateWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == kMsgSelect && self) {
        // A click selects by row on the page that was on screen; ignore it if
        // the list has changed since (the generation moved on).
        const auto row = static_cast<std::size_t>(wParam);
        const auto index = self->pageStart_ + row;
        if (self->shown_ && lParam == static_cast<LPARAM>(self->generation_) &&
            row < self->visibleCount_ && index < self->candidates_.size() && self->onSelection_) {
            self->onSelection_(index);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tekito::tsf
