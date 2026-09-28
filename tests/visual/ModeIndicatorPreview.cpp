// Shows the mode indicator for each mode under a caret on a plain page and
// saves a screenshot of each, for reviewing its look without registering the
// input method.
//
//   tekito_mode_indicator_preview.exe <output folder>
//
// TEKITO_PREVIEW_THEME=dark|light overrides the apps' theme.
#include "Tsf/ModeIndicator.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

void Pump(ULONGLONG milliseconds) {
    MSG msg;
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < milliseconds) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
}

bool g_dark = false;

LRESULT CALLBACK PageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH page = CreateSolidBrush(g_dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
        FillRect(dc, &client, page);
        DeleteObject(page);
        // The caret the indicator sits under.
        RECT caret{40, 30, 42, 54};
        HBRUSH ink = CreateSolidBrush(g_dark ? RGB(230, 230, 230) : RGB(28, 30, 34));
        FillRect(dc, &caret, ink);
        DeleteObject(ink);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void Save(const RECT& rect, const std::filesystem::path& path) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
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
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path folder = argc > 1 ? argv[1] : L"mode-indicator";
    std::filesystem::create_directories(folder);
    wchar_t theme[16]{};
    g_dark = GetEnvironmentVariableW(L"TEKITO_PREVIEW_THEME", theme, 16) > 0 && wcscmp(theme, L"dark") == 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = &PageProc;
    windowClass.lpszClassName = L"TekitoModeIndicatorPage";
    RegisterClassExW(&windowClass);
    HWND page = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"",
                                WS_POPUP, 200, 200, 180, 130, nullptr, nullptr, instance, nullptr);
    ShowWindow(page, SW_SHOWNOACTIVATE);
    UpdateWindow(page);
    Pump(200);

    RECT pageRect{};
    GetWindowRect(page, &pageRect);
    const RECT caret{pageRect.left + 40, pageRect.top + 30, pageRect.left + 42, pageRect.top + 54};
    tekito::tsf::ModeIndicator indicator;
    const struct {
        tekito::InputMode mode;
        const wchar_t* name;
    } modes[] = {{tekito::InputMode::Convert, L"auto"},
                 {tekito::InputMode::Direct, L"direct"},
                 {tekito::InputMode::Japanese, L"japanese"}};
    for (const auto& mode : modes) {
        indicator.Show(instance, caret, mode.mode);
        Pump(250);
        Save(pageRect, folder / (std::wstring(mode.name) + (g_dark ? L"-dark.bmp" : L"-light.bmp")));
    }
    // It fades and hides on its own.
    Pump(1400);
    const bool hidden = !IsWindowVisible(FindWindowW(L"TekitoModeIndicator", nullptr));
    indicator.Shutdown();
    DestroyWindow(page);
    return hidden ? 0 : 4;
}
