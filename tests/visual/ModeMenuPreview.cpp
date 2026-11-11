// Shows the taskbar mode button's menu (ModeMenu) over a backdrop and saves a
// screenshot of the screen around it, so the capture has the acrylic, the
// shadow and the rounded corners as a user sees them.
//
//   tekito_mode_menu_preview.exe <out.bmp> [ja] [keys] [secret]
//
// `ja` shows the Japanese labels and the Japanese and Mix modes; `keys`
// moves the focus with the arrow keys first (the focus ring); `secret` shows
// secret mode on. TEKITO_PREVIEW_THEME=dark|light picks the theme.
// The menu opens where TEKITO_PREVIEW_POINT ("x,y") says, not at the
// pointer, so the preview never moves the user's pointer.
#include "Tsf/ModeMenu.h"

#include "assets/tekito_resource.h"

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace {

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client;
        GetClientRect(hwnd, &client);
        const COLORREF stripes[] = {RGB(255, 94, 98), RGB(255, 170, 64), RGB(76, 201, 140), RGB(64, 156, 255),
                                    RGB(160, 100, 255)};
        const int stripe = (client.right + 4) / 5;
        for (int i = 0; i < 5; ++i) {
            RECT band{i * stripe, 0, (i + 1) * stripe, client.bottom};
            HBRUSH brush = CreateSolidBrush(stripes[i]);
            FillRect(dc, &band, brush);
            DeleteObject(brush);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool SaveScreenRect(RECT rect, const wchar_t* path) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    BITMAPINFOHEADER header{sizeof(header), width, -height, 1, 32, BI_RGB};
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    BITMAPINFO info{};
    info.bmiHeader = header;
    GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    FILE* out = nullptr;
    _wfopen_s(&out, path, L"wb");
    if (!out) return false;
    fwrite(&file, sizeof(file), 1, out);
    fwrite(&header, sizeof(header), 1, out);
    fwrite(pixels.data(), pixels.size(), 1, out);
    fclose(out);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: tekito_mode_menu_preview <out.bmp> [ja] [keys] [secret]\n");
        return 2;
    }
    bool japanese = false;
    bool keys = false;
    bool secret = false;
    for (int i = 2; i < argc; ++i) {
        japanese = japanese || wcscmp(argv[i], L"ja") == 0;
        keys = keys || wcscmp(argv[i], L"keys") == 0;
        secret = secret || wcscmp(argv[i], L"secret") == 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW backdropClass{sizeof(backdropClass)};
    backdropClass.lpfnWndProc = BackdropProc;
    backdropClass.hInstance = instance;
    backdropClass.lpszClassName = L"TekitoMenuPreviewBackdrop";
    RegisterClassExW(&backdropClass);
    const HWND backdrop = CreateWindowExW(WS_EX_TOOLWINDOW, backdropClass.lpszClassName, L"", WS_POPUP, 200, 200, 520,
                                          520, nullptr, nullptr, instance, nullptr);
    ShowWindow(backdrop, SW_SHOW);
    UpdateWindow(backdrop);
    SetEnvironmentVariableW(L"TEKITO_PREVIEW_POINT", L"460,660");

    using Kind = tekito::tsf::ModeMenuItem::Kind;
    std::vector<tekito::tsf::ModeMenuItem> items{
        {Kind::Mode, 1, L"Auto", IDI_AUTO, 0, !japanese},
        {Kind::Mode, 2, L"Direct", IDI_DIRECT, 0, false},
    };
    if (japanese) {
        items.push_back({Kind::Mode, 4, L"日本語", IDI_JAPANESE, 0, true});
        items.push_back({Kind::Mode, 5, L"まぜ打ち", IDI_MIXED, 0, false});
    }
    items.push_back({Kind::Separator});
    items.push_back({Kind::Toggle, 6, japanese ? L"シークレットモード" : L"Secret mode", 0, 0xED1A, secret});
    items.push_back({Kind::Command, 3, japanese ? L"設定" : L"Settings", 0, 0xE713, false});

    const std::wstring out = argv[1];
    bool saved = false;
    std::thread capture([&] {
        Sleep(400);
        const HWND menu = FindWindowW(L"TekitoModeMenu", nullptr);
        if (!menu) return;
        if (keys) {
            PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
            PostMessageW(menu, WM_KEYDOWN, VK_DOWN, 0);
        }
        Sleep(500);
        RECT rect;
        GetWindowRect(menu, &rect);
        InflateRect(&rect, 32, 32);
        saved = SaveScreenRect(rect, out.c_str());
        PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
    });
    UINT chosen = 0;
    tekito::tsf::ModeMenu::Show(
        instance, std::move(items), [&](UINT id) { chosen = id; }, [&](UINT) { return secret; });
    capture.join();
    DestroyWindow(backdrop);
    wprintf(saved ? L"Saved %ls (chosen %u)\n" : L"No capture (%ls)\n", out.c_str(), chosen);
    return saved ? 0 : 1;
}
