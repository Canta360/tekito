// Records the README typing demo: a short line typed key by key into a plain
// page, with the real engine producing the candidate list and the real
// typing state machine deciding what Space and punctuation do. Each step is
// captured as a BMP frame; scripts/make-demo-animation.py assembles them.
//
//   tekito_typing_demo.exe <output folder>
//
// Needs the Data Packs (TEKITO_DATA_PACK_DIR) and a visible desktop.
#include "Core/ConversionEngine.h"
#include "Core/InputStateMachine.h"
#include "Core/UserDictionary.h"
#include "Tsf/CandidateWindow.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 300;
constexpr int kMargin = 28;
constexpr int kLineTop = 34;

struct Page {
    std::wstring committed;    // text before the word being typed
    std::wstring composition;  // the word being typed, underlined
    bool caret{true};
};

Page g_page;
HFONT g_font = nullptr;

// Screen rectangle of the composition, where the candidate list anchors.
RECT CompositionRect(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, g_font);
    SIZE before{};
    SIZE word{};
    GetTextExtentPoint32W(dc, g_page.committed.c_str(), static_cast<int>(g_page.committed.size()), &before);
    GetTextExtentPoint32W(dc, g_page.composition.c_str(), static_cast<int>(g_page.composition.size()), &word);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    POINT topLeft{kMargin + before.cx, kLineTop};
    POINT bottomRight{kMargin + before.cx + word.cx, kLineTop + metrics.tmHeight};
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);
    return {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

void PaintPage(HWND hwnd, HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);
    HBRUSH page = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, page);
    DeleteObject(page);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(28, 30, 34));
    HGDIOBJ old = SelectObject(dc, g_font);
    const std::wstring line = g_page.committed + g_page.composition;
    TextOutW(dc, kMargin, kLineTop, line.c_str(), static_cast<int>(line.size()));

    SIZE before{};
    SIZE all{};
    GetTextExtentPoint32W(dc, g_page.committed.c_str(), static_cast<int>(g_page.committed.size()), &before);
    GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &all);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int baseline = kLineTop + metrics.tmHeight + 1;
    if (!g_page.composition.empty()) {
        // Dotted underline, as TSF hosts draw TEKITO's composition.
        HPEN pen = CreatePen(PS_DOT, 1, RGB(28, 30, 34));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, kMargin + before.cx, baseline, nullptr);
        LineTo(dc, kMargin + all.cx, baseline);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    if (g_page.caret) {
        RECT caret{kMargin + all.cx + 1, kLineTop, kMargin + all.cx + 2, kLineTop + metrics.tmHeight};
        HBRUSH ink = CreateSolidBrush(RGB(28, 30, 34));
        FillRect(dc, &caret, ink);
        DeleteObject(ink);
    }
    SelectObject(dc, old);
}

LRESULT CALLBACK PageProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        PaintPage(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

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

bool SaveScreenRect(const RECT& rect, const std::filesystem::path& path) {
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
    return static_cast<bool>(output);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path outputFolder = argc > 1 ? argv[1] : L"typing-demo";
    std::filesystem::create_directories(outputFolder);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    g_font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         L"Segoe UI");
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = &PageProc;
    windowClass.lpszClassName = L"TekitoTypingDemoPage";
    RegisterClassExW(&windowClass);
    HWND page = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                windowClass.lpszClassName, L"", WS_POPUP, 160, 200, kWidth, kHeight,
                                nullptr, nullptr, instance, nullptr);
    ShowWindow(page, SW_SHOWNOACTIVATE);

    tekito::tsf::CandidateWindow window;
    if (!window.Initialize(instance, [](std::size_t) {})) return 1;
    // TEKITO_PREVIEW_LANGUAGE=ja records the demo with the Japanese tags.
    wchar_t language[8]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_LANGUAGE", language, 8) > 0 && wcscmp(language, L"ja") == 0) {
        window.SetJapanese(true);
    }
    tekito::UserDictionary dictionary;
    const auto engine = tekito::CreateDefaultConversionEngine(dictionary);
    if (!engine) return 1;

    std::ofstream timing(outputFolder / L"frames.txt");
    int frameNumber = 0;
    const auto capture = [&](int milliseconds) {
        InvalidateRect(page, nullptr, FALSE);
        UpdateWindow(page);
        Pump(220);  // let the list finish its animation
        RECT rect{};
        GetWindowRect(page, &rect);
        char name[32]{};
        sprintf_s(name, "frame%03d.bmp", frameNumber++);
        SaveScreenRect(rect, outputFolder / name);
        timing << name << ' ' << milliseconds << '\n';
    };
    const auto show = [&](const tekito::InputStateMachine& state) {
        window.Show(CompositionRect(page), state.Candidates(), state.SelectedIndex(),
                    state.PageStart(), state.VisibleCount());
    };

    g_page.committed = L"Hi Sam, ";
    capture(900);
    const std::wstring words[] = {L"thnaks", L"for", L"teh", L"reveiw"};
    for (std::size_t w = 0; w < std::size(words); ++w) {
        tekito::InputStateMachine state;
        for (std::size_t length = 1; length <= words[w].size(); ++length) {
            g_page.composition = words[w].substr(0, length);
            tekito::ConversionRequest request;
            request.rawText = g_page.composition;
            request.context.precedingText = g_page.committed;
            state.BeginOrUpdate(g_page.composition, engine->Convert(request).candidates);
            show(state);
            capture(length == words[w].size() ? 700 : 160);
        }
        const bool last = w + 1 == std::size(words);
        const auto action = last ? state.OnPunctuation(L'.', tekito::PunctuationRole::SentenceTerminal)
                                 : state.OnSpace();
        wprintf(L"%ls -> [%ls]\n", words[w].c_str(), action.text.c_str());
        window.Hide();
        g_page.committed += action.text;
        g_page.composition.clear();
        capture(last ? 2400 : 450);
    }
    window.Hide();
    DestroyWindow(page);
    return 0;
}
