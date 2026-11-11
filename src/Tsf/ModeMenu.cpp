#include <unknwn.h>

#include "Tsf/ModeMenu.h"

#include "assets/tekito_resource.h"

#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>

namespace tekito::tsf {
namespace {

constexpr wchar_t kClassName[] = L"TekitoModeMenu";

// Layout, in DIPs (Windows 11's menus).
constexpr float kPadding = 4.0f;
constexpr float kRowHeight = 36.0f;
constexpr float kRowRadius = 4.0f;
constexpr float kSeparatorHeight = 9.0f;
constexpr float kIconLeft = 12.0f;
constexpr float kIconSize = 16.0f;
constexpr float kLabelLeft = 40.0f;
constexpr float kRightPadding = 12.0f;
constexpr float kLabelGap = 24.0f;
constexpr float kMinWidth = 200.0f;
constexpr float kFontSize = 14.0f;
constexpr float kGlyphSize = 16.0f;
constexpr float kSwitchWidth = 40.0f;
constexpr float kSwitchHeight = 20.0f;
// From the taskbar or the screen's edge.
constexpr float kScreenGap = 12.0f;
// How often the menu looks whether another window took the foreground.
constexpr DWORD kPollMilliseconds = 50;

constexpr wchar_t kGlyphCheck = 0xE73E;

DWORD ReadPersonalizeValue(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD size = sizeof(value);
    const auto result = RegGetValueW(HKEY_CURRENT_USER,
                                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", name,
                                     RRF_RT_REG_DWORD, nullptr, &value, &size);
    return result == ERROR_SUCCESS ? value : fallback;
}

bool IsHighContrast() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

D2D1_COLOR_F Rgba(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }

struct Theme {
    bool dark{false};
    // Acrylic behind the menu (Windows 11 with transparency effects on).
    bool acrylic{false};
    D2D1_COLOR_F background{};
    D2D1_COLOR_F text{};
    D2D1_COLOR_F secondary{};
    D2D1_COLOR_F hover{};
    D2D1_COLOR_F pressed{};
    D2D1_COLOR_F current{};
    D2D1_COLOR_F separator{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F onAccent{};
};

// The accent as Windows 11 shows it in each theme: a darker shade on light,
// a lighter one on dark.
D2D1_COLOR_F Accent(bool dark) {
    try {
        using winrt::Windows::UI::ViewManagement::UIColorType;
        const auto color = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            dark ? UIColorType::AccentLight2 : UIColorType::AccentDark1);
        return Rgba(color.R / 255.0f, color.G / 255.0f, color.B / 255.0f);
    } catch (...) {
        return dark ? Rgba(0.6f, 0.92f, 1.0f) : Rgba(0.0f, 0.37f, 0.72f);
    }
}

Theme ReadTheme() {
    Theme theme;
    theme.dark = ReadPersonalizeValue(L"AppsUseLightTheme", 1) == 0;
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    wchar_t preview[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_THEME", preview, 16) > 0) theme.dark = wcscmp(preview, L"dark") == 0;
#endif
    OSVERSIONINFOW version{};
    // Acrylic for popups (DWMSBT_TRANSIENTWINDOW) arrived in Windows 11 22H2.
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    version.dwOSVersionInfoSize = sizeof(version);
    if (const auto get = reinterpret_cast<RtlGetVersion>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
        get(&version);
    }
    theme.acrylic = version.dwBuildNumber >= 22621 && ReadPersonalizeValue(L"EnableTransparency", 1) != 0;
    if (theme.dark) {
        theme.background = Rgba(0.173f, 0.173f, 0.173f);
        theme.text = Rgba(1.0f, 1.0f, 1.0f);
        theme.secondary = Rgba(1.0f, 1.0f, 1.0f, 0.786f);
        theme.hover = Rgba(1.0f, 1.0f, 1.0f, 0.061f);
        theme.pressed = Rgba(1.0f, 1.0f, 1.0f, 0.042f);
        theme.current = Rgba(1.0f, 1.0f, 1.0f, 0.084f);
        theme.separator = Rgba(1.0f, 1.0f, 1.0f, 0.084f);
        theme.onAccent = Rgba(0.0f, 0.0f, 0.0f);
    } else {
        theme.background = Rgba(0.976f, 0.976f, 0.976f);
        theme.text = Rgba(0.0f, 0.0f, 0.0f, 0.896f);
        theme.secondary = Rgba(0.0f, 0.0f, 0.0f, 0.606f);
        theme.hover = Rgba(0.0f, 0.0f, 0.0f, 0.037f);
        theme.pressed = Rgba(0.0f, 0.0f, 0.0f, 0.024f);
        theme.current = Rgba(0.0f, 0.0f, 0.0f, 0.058f);
        theme.separator = Rgba(0.0f, 0.0f, 0.0f, 0.08f);
        theme.onAccent = Rgba(1.0f, 1.0f, 1.0f);
    }
    theme.accent = Accent(theme.dark);
    return theme;
}

// The TEKITO typeface (M PLUS 1), from this DLL, as the candidate window
// loads it: in a private collection of the menu's own DirectWrite factory.
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
    if (FAILED(font.loader->CreateInMemoryFontFileReference(factory5.get(), bytes, size, nullptr, file.put())) ||
        FAILED(factory5->CreateFontSetBuilder(builder.put())) || FAILED(builder->AddFontFile(file.get())) ||
        FAILED(builder->CreateFontSet(set.put())) ||
        FAILED(factory5->CreateFontCollectionFromFontSet(set.get(), font.collection.put()))) {
        font.collection = nullptr;
    }
    return font;
}

const wchar_t* IconFontFamily(IDWriteFactory* factory) {
    winrt::com_ptr<IDWriteFontCollection> fonts;
    if (!factory || FAILED(factory->GetSystemFontCollection(fonts.put(), FALSE))) return nullptr;
    for (const wchar_t* family : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists) return family;
    }
    return nullptr;
}

struct Menu {
    HINSTANCE instance{nullptr};
    HWND hwnd{nullptr};
    std::vector<ModeMenuItem> items;
    std::vector<D2D1_RECT_F> rows;
    std::function<bool(UINT)> checked;
    std::function<void(UINT)> choose;
    Theme theme;
    float scale{1.0f};
    float width{0.0f};
    float height{0.0f};
    int hover{-1};
    int pressed{-1};
    int focus{-1};
    // The focus ring shows once the keyboard is used.
    bool keyboard{false};
    bool active{false};
    bool tracking{false};
    bool done{false};
    UINT chosen{0};

    winrt::com_ptr<ID2D1Factory> d2d;
    winrt::com_ptr<ID2D1HwndRenderTarget> target;
    winrt::com_ptr<IDWriteFactory> dwrite;
    BrandFont brand;
    winrt::com_ptr<IDWriteTextFormat> label;
    winrt::com_ptr<IDWriteTextFormat> glyph;
    std::vector<winrt::com_ptr<IWICBitmapSource>> iconSources;
    std::vector<winrt::com_ptr<ID2D1Bitmap>> icons;
};

thread_local Menu* g_menu = nullptr;

winrt::com_ptr<IDWriteTextFormat> MakeFormat(IDWriteFactory* factory, IDWriteFontCollection* collection,
                                             const wchar_t* family, float size, DWRITE_TEXT_ALIGNMENT alignment) {
    winrt::com_ptr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(family, collection, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", format.put()))) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    return format;
}

float TextWidth(Menu& menu, const std::wstring& text) {
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!menu.label || FAILED(menu.dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                                            menu.label.get(), 10000.0f, 100.0f, layout.put()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

bool Selectable(const ModeMenuItem& item) { return item.kind != ModeMenuItem::Kind::Separator; }

void Layout(Menu& menu) {
    float widest = 0.0f;
    bool toggle = false;
    for (const auto& item : menu.items) {
        if (!Selectable(item)) continue;
        widest = std::max(widest, TextWidth(menu, item.label));
        toggle = toggle || item.kind == ModeMenuItem::Kind::Toggle;
    }
    const float right = toggle ? kSwitchWidth : kGlyphSize;
    menu.width = std::max(kMinWidth, kPadding * 2 + kLabelLeft + std::ceil(widest) + kLabelGap + right + kRightPadding);
    float y = kPadding;
    menu.rows.clear();
    for (const auto& item : menu.items) {
        const float h = item.kind == ModeMenuItem::Kind::Separator ? kSeparatorHeight : kRowHeight;
        menu.rows.push_back(D2D1::RectF(kPadding, y, menu.width - kPadding, y + h));
        y += h;
    }
    menu.height = y + kPadding;
}

// The mode icons in the menu's theme (the dark ones on dark).
int ThemedIcon(int icon, bool dark) {
    if (!dark) return icon;
    if (icon == IDI_JAPANESE) return IDI_JAPANESE_DARK;
    if (icon == IDI_DIRECT) return IDI_DIRECT_DARK;
    return icon;
}

void LoadIcons(Menu& menu) {
    menu.iconSources.assign(menu.items.size(), nullptr);
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) {
        return;
    }
    const int size = static_cast<int>(std::lround(kIconSize * menu.scale));
    for (std::size_t i = 0; i < menu.items.size(); ++i) {
        const auto& item = menu.items[i];
        if (item.kind != ModeMenuItem::Kind::Mode || item.icon == 0) continue;
        const auto icon = static_cast<HICON>(LoadImageW(menu.instance, MAKEINTRESOURCEW(ThemedIcon(item.icon, menu.theme.dark)),
                                                        IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (!icon) continue;
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::com_ptr<IWICFormatConverter> converter;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, bitmap.put())) &&
            SUCCEEDED(wic->CreateFormatConverter(converter.put())) &&
            SUCCEEDED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            menu.iconSources[i] = converter;
        }
        DestroyIcon(icon);
    }
}

bool EnsureTarget(Menu& menu) {
    if (menu.target) return true;
    RECT client{};
    GetClientRect(menu.hwnd, &client);
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const auto hwndProperties = D2D1::HwndRenderTargetProperties(
        menu.hwnd, D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom)));
    if (FAILED(menu.d2d->CreateHwndRenderTarget(properties, hwndProperties, menu.target.put()))) return false;
    menu.target->SetDpi(96.0f * menu.scale, 96.0f * menu.scale);
    // On acrylic the text has no opaque background for ClearType.
    menu.target->SetTextAntialiasMode(menu.theme.acrylic ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE
                                                         : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
    menu.icons.assign(menu.items.size(), nullptr);
    for (std::size_t i = 0; i < menu.iconSources.size(); ++i) {
        if (menu.iconSources[i]) menu.target->CreateBitmapFromWicBitmap(menu.iconSources[i].get(), menu.icons[i].put());
    }
    return true;
}

void DrawSwitch(Menu& menu, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& row, bool on) {
    const float cy = (row.top + row.bottom) / 2;
    const float right = row.right - kRightPadding;
    const auto track = D2D1::RectF(right - kSwitchWidth, cy - kSwitchHeight / 2, right, cy + kSwitchHeight / 2);
    const float r = kSwitchHeight / 2;
    if (on) {
        brush->SetColor(menu.theme.accent);
        menu.target->FillRoundedRectangle(D2D1::RoundedRect(track, r, r), brush);
        brush->SetColor(menu.theme.onAccent);
        menu.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.right - r, cy), 7.0f, 7.0f), brush);
    } else {
        brush->SetColor(menu.theme.secondary);
        const auto inset = D2D1::RectF(track.left + 0.5f, track.top + 0.5f, track.right - 0.5f, track.bottom - 0.5f);
        menu.target->DrawRoundedRectangle(D2D1::RoundedRect(inset, r - 0.5f, r - 0.5f), brush, 1.0f);
        menu.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(track.left + r, cy), 6.0f, 6.0f), brush);
    }
}

void Paint(Menu& menu) {
    if (!EnsureTarget(menu)) return;
    auto* target = menu.target.get();
    target->BeginDraw();
    target->Clear(menu.theme.acrylic ? Rgba(0, 0, 0, 0) : menu.theme.background);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    target->CreateSolidColorBrush(menu.theme.text, brush.put());
    for (std::size_t i = 0; i < menu.items.size(); ++i) {
        const auto& item = menu.items[i];
        const auto& row = menu.rows[i];
        const int index = static_cast<int>(i);
        if (item.kind == ModeMenuItem::Kind::Separator) {
            const float y = std::floor((row.top + row.bottom) / 2) + 0.5f;
            brush->SetColor(menu.theme.separator);
            target->DrawLine(D2D1::Point2F(row.left - kPadding, y), D2D1::Point2F(row.right + kPadding, y), brush.get(), 1.0f);
            continue;
        }
        const auto rounded = D2D1::RoundedRect(row, kRowRadius, kRowRadius);
        const bool current = item.kind == ModeMenuItem::Kind::Mode && item.checked;
        if (current) {
            brush->SetColor(menu.theme.current);
            target->FillRoundedRectangle(rounded, brush.get());
        }
        if (menu.pressed == index && menu.hover == index) {
            brush->SetColor(menu.theme.pressed);
            target->FillRoundedRectangle(rounded, brush.get());
        } else if (menu.hover == index) {
            brush->SetColor(menu.theme.hover);
            target->FillRoundedRectangle(rounded, brush.get());
        }
        if (menu.keyboard && menu.focus == index) {
            brush->SetColor(menu.theme.text);
            const auto ring = D2D1::RectF(row.left + 1, row.top + 1, row.right - 1, row.bottom - 1);
            target->DrawRoundedRectangle(D2D1::RoundedRect(ring, kRowRadius - 1, kRowRadius - 1), brush.get(), 2.0f);
        }
        const float cy = (row.top + row.bottom) / 2;
        const auto iconBox = D2D1::RectF(row.left + kIconLeft, cy - kIconSize / 2, row.left + kIconLeft + kIconSize,
                                         cy + kIconSize / 2);
        if (i < menu.icons.size() && menu.icons[i]) {
            target->DrawBitmap(menu.icons[i].get(), iconBox);
        } else if (item.glyph && menu.glyph) {
            brush->SetColor(menu.theme.text);
            target->DrawText(&item.glyph, 1, menu.glyph.get(), iconBox, brush.get());
        }
        brush->SetColor(menu.theme.text);
        if (menu.label) {
            target->DrawText(item.label.c_str(), static_cast<UINT32>(item.label.size()), menu.label.get(),
                             D2D1::RectF(row.left + kLabelLeft, row.top, row.right - kRightPadding, row.bottom),
                             brush.get());
        }
        if (current && menu.glyph) {
            brush->SetColor(menu.theme.accent);
            const auto box = D2D1::RectF(row.right - kRightPadding - kGlyphSize, cy - kGlyphSize / 2,
                                         row.right - kRightPadding, cy + kGlyphSize / 2);
            target->DrawText(&kGlyphCheck, 1, menu.glyph.get(), box, brush.get());
        }
        if (item.kind == ModeMenuItem::Kind::Toggle) DrawSwitch(menu, brush.get(), row, item.checked);
    }
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
        menu.target = nullptr;
        menu.icons.clear();
    }
}

int HitTest(const Menu& menu, LPARAM lParam) {
    const float x = static_cast<float>(static_cast<short>(LOWORD(lParam))) / menu.scale;
    const float y = static_cast<float>(static_cast<short>(HIWORD(lParam))) / menu.scale;
    for (std::size_t i = 0; i < menu.rows.size(); ++i) {
        const auto& row = menu.rows[i];
        if (Selectable(menu.items[i]) && x >= row.left && x < row.right && y >= row.top && y < row.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Activate(Menu& menu, int index) {
    if (index < 0 || index >= static_cast<int>(menu.items.size()) || !Selectable(menu.items[index])) return;
    auto& item = menu.items[index];
    if (item.kind == ModeMenuItem::Kind::Toggle) {
        if (menu.choose) menu.choose(item.id);
        item.checked = menu.checked ? menu.checked(item.id) : !item.checked;
        InvalidateRect(menu.hwnd, nullptr, FALSE);
        return;
    }
    menu.chosen = item.id;
    menu.done = true;
}

void MoveFocus(Menu& menu, int step) {
    const int count = static_cast<int>(menu.items.size());
    int index = menu.focus;
    for (int tries = 0; tries < count; ++tries) {
        index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
        if (Selectable(menu.items[index])) break;
    }
    menu.focus = index;
    menu.keyboard = true;
    InvalidateRect(menu.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    Menu* menu = g_menu;
    if (!menu || menu->hwnd != hwnd) return DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        Paint(*menu);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!menu->tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            menu->tracking = TrackMouseEvent(&track) != FALSE;
        }
        const int hit = HitTest(*menu, lParam);
        if (hit != menu->hover) {
            menu->hover = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        menu->tracking = false;
        menu->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        menu->pressed = HitTest(*menu, lParam);
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        const int hit = HitTest(*menu, lParam);
        const int pressed = std::exchange(menu->pressed, -1);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (hit >= 0 && hit == pressed) Activate(*menu, hit);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_UP: MoveFocus(*menu, -1); return 0;
        case VK_DOWN: MoveFocus(*menu, 1); return 0;
        case VK_HOME: menu->focus = -1; MoveFocus(*menu, 1); return 0;
        case VK_END: menu->focus = -1; MoveFocus(*menu, -1); return 0;
        case VK_RETURN:
        case VK_SPACE: Activate(*menu, menu->focus); return 0;
        case VK_ESCAPE: menu->done = true; return 0;
        default: break;
        }
        break;
    case WM_SYSKEYDOWN:
    case WM_CANCELMODE:
        menu->done = true;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            if (menu->active) menu->done = true;
        } else {
            menu->active = true;
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterClassOnce(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    registered = RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return registered;
}

// Above the taskbar, centered on the pointer; beside it when the taskbar is
// on a side; above the pointer elsewhere (a floating language bar).
POINT Place(POINT cursor, int width, int height, int gap) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info);
    const RECT work = info.rcWork;
    int x = cursor.x - width / 2;
    int y = cursor.y - height - gap / 3;
    if (cursor.y >= work.bottom) {
        y = work.bottom - height - gap;
    } else if (cursor.y < work.top) {
        y = work.top + gap;
    } else if (cursor.x < work.left) {
        x = work.left + gap;
        y = cursor.y - height / 2;
    } else if (cursor.x >= work.right) {
        x = work.right - width - gap;
        y = cursor.y - height / 2;
    } else if (y < work.top) {
        y = cursor.y + gap / 3;
    }
    x = std::clamp(x, static_cast<int>(work.left) + gap, std::max(static_cast<int>(work.left) + gap,
                                                                  static_cast<int>(work.right) - width - gap));
    y = std::clamp(y, static_cast<int>(work.top) + gap, std::max(static_cast<int>(work.top) + gap,
                                                                 static_cast<int>(work.bottom) - height - gap));
    return {x, y};
}

}  // namespace

bool ModeMenu::Available() { return !IsHighContrast(); }

bool ModeMenu::IsOpen() { return g_menu != nullptr; }

void ModeMenu::Close() {
    if (g_menu) g_menu->done = true;
}

void ModeMenu::Show(HINSTANCE instance, std::vector<ModeMenuItem> items, const std::function<void(UINT)>& choose,
                    const std::function<bool(UINT)>& checked) {
    if (g_menu || items.empty() || !RegisterClassOnce(instance)) return;
    // In the pointer's monitor's pixels, whatever the app's own DPI handling.
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    POINT cursor{};
    GetCursorPos(&cursor);
#ifdef TEKITO_CANDIDATE_WINDOW_PREVIEW
    wchar_t point[32]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_POINT", point, 32) > 0) swscanf_s(point, L"%ld,%ld", &cursor.x, &cursor.y);
#endif

    Menu menu;
    menu.instance = instance;
    menu.items = std::move(items);
    menu.choose = choose;
    menu.checked = checked;
    menu.theme = ReadTheme();
    for (std::size_t i = 0; i < menu.items.size(); ++i) {
        if (menu.items[i].kind == ModeMenuItem::Kind::Mode && menu.items[i].checked) menu.focus = static_cast<int>(i);
    }
    g_menu = &menu;

    menu.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"", WS_POPUP, cursor.x, cursor.y, 1, 1,
                                nullptr, nullptr, instance, nullptr);
    const bool ready =
        menu.hwnd && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, menu.d2d.put())) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(menu.dwrite.put())));
    if (ready) {
        const UINT dpi = GetDpiForWindow(menu.hwnd);
        menu.scale = static_cast<float>(dpi ? dpi : 96) / 96.0f;
        menu.brand = LoadBrandFont(menu.dwrite.get(), instance);
        menu.label = MakeFormat(menu.dwrite.get(), menu.brand.collection.get(),
                                menu.brand.collection ? L"M PLUS 1" : L"Segoe UI Variable Text", kFontSize,
                                DWRITE_TEXT_ALIGNMENT_LEADING);
        if (const wchar_t* family = IconFontFamily(menu.dwrite.get())) {
            menu.glyph = MakeFormat(menu.dwrite.get(), nullptr, family, kGlyphSize, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Layout(menu);
        LoadIcons(menu);

        const int width = static_cast<int>(std::ceil(menu.width * menu.scale));
        const int height = static_cast<int>(std::ceil(menu.height * menu.scale));
        const POINT at = Place(cursor, width, height, static_cast<int>(std::lround(kScreenGap * menu.scale)));
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(menu.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        const BOOL dark = menu.theme.dark;
        DwmSetWindowAttribute(menu.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        // Extending the frame gives the popup a DWM shadow, and the acrylic
        // shows through where the menu draws nothing.
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(menu.hwnd, &margins);
        if (menu.theme.acrylic) {
            const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
            menu.theme.acrylic = SUCCEEDED(
                DwmSetWindowAttribute(menu.hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
        }
        SetWindowPos(menu.hwnd, HWND_TOPMOST, at.x, at.y, width, height, SWP_SHOWWINDOW);
        SetForegroundWindow(menu.hwnd);
        UpdateWindow(menu.hwnd);

        // Until something is chosen, or the menu is left: Esc, a click on
        // another window, another window coming to the front.
        const HWND front = GetForegroundWindow();
        while (!menu.done) {
            MsgWaitForMultipleObjectsEx(0, nullptr, kPollMilliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message;
            while (!menu.done && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    menu.done = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const HWND now = GetForegroundWindow();
            if (!IsWindow(menu.hwnd) || (now != menu.hwnd && now != front)) menu.done = true;
        }
    }
    if (menu.hwnd) {
        ShowWindow(menu.hwnd, SW_HIDE);
        DestroyWindow(menu.hwnd);
    }
    menu.target = nullptr;
    menu.icons.clear();
    menu.label = nullptr;
    menu.brand.collection = nullptr;
    if (menu.brand.loader) {
        winrt::com_ptr<IDWriteFactory5> factory5;
        menu.dwrite->QueryInterface(IID_PPV_ARGS(factory5.put()));
        if (factory5) factory5->UnregisterFontFileLoader(menu.brand.loader.get());
    }
    g_menu = nullptr;
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    if (menu.chosen && choose) choose(menu.chosen);
}

}  // namespace tekito::tsf
