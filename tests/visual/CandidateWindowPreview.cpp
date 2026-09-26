// Standalone CandidateWindow renderer for visual review without registering
// the TSF. Shows sample candidates over a backdrop window and saves a
// screenshot of the composited screen, so the capture includes the backdrop
// blur, the shadow window and the rounded corners exactly as a user would
// see them.
//
//   tekito_candidate_window_preview.exe <out.bmp> [stripes|doc|paper|dusk] [short|long] [glide] [click]
//
// `stripes` puts saturated color bands behind the glass to show the blur;
// `doc` is a plain page of text, the common case; `paper` is a soft light
// background for product shots; `dusk` is a dark gradient
// wallpaper. `long` shows more
// candidates than fit on a page, which brings up the page indicator.
// `glide` moves the selection two rows down and captures mid-animation.
// `click` clicks row 4 and reports the index the host callback received.
//
// TEKITO_PREVIEW_STYLE=simple renders the simple candidate window style.
// Environment overrides (preview builds only): TEKITO_PREVIEW_THEME=dark|light,
// TEKITO_PREVIEW_NO_COMPOSITION=1 (HWND fallback renderer),
// TEKITO_PREVIEW_HIGH_CONTRAST=1 (high-contrast palette from system colors).
#include "Tsf/CandidateWindow.h"

#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr int kCaptureMargin = 40;
bool g_documentBackdrop = false;
bool g_duskBackdrop = false;
bool g_paperBackdrop = false;

// Dark slate-to-teal gradient, a calm dark wallpaper.
void PaintDusk(HDC dc, const RECT& client) {
    const int height = client.bottom - client.top;
    for (int y = 0; y < height; ++y) {
        const double f = static_cast<double>(y) / static_cast<double>(height > 1 ? height - 1 : 1);
        const auto mix = [f](int a, int b) { return static_cast<BYTE>(a + (b - a) * f); };
        HBRUSH brush = CreateSolidBrush(RGB(mix(22, 92), mix(34, 118), mix(42, 126)));
        RECT line{client.left, y, client.right, y + 1};
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

// Soft light gray-blue, top to bottom.
void PaintPaper(HDC dc, const RECT& client) {
    const int height = client.bottom - client.top;
    for (int y = 0; y < height; ++y) {
        const double f = static_cast<double>(y) / static_cast<double>(height > 1 ? height - 1 : 1);
        const auto mix = [f](int a, int b) { return static_cast<BYTE>(a + (b - a) * f); };
        HBRUSH brush = CreateSolidBrush(RGB(mix(244, 226), mix(246, 232), mix(250, 241)));
        RECT line{client.left, y, client.right, y + 1};
        FillRect(dc, &line, brush);
        DeleteObject(brush);
    }
}

void PaintStripes(HDC dc, const RECT& client) {
    const COLORREF stripes[] = {RGB(255, 94, 98), RGB(255, 170, 64), RGB(76, 201, 140),
                                RGB(64, 156, 255), RGB(160, 100, 255)};
    const int stripeWidth = (client.right + 4) / 5;
    for (int i = 0; i < 5; ++i) {
        RECT stripe{i * stripeWidth, 0, (i + 1) * stripeWidth, client.bottom};
        HBRUSH brush = CreateSolidBrush(stripes[i]);
        FillRect(dc, &stripe, brush);
        DeleteObject(brush);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 30));
    HFONT font = CreateFontW(-56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT textRect = client;
    DrawTextW(dc, L"TEKITO glass", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

void PaintDocument(HDC dc, const RECT& client) {
    HBRUSH page = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, page);
    DeleteObject(page);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 30, 30));
    HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    const wchar_t* lines[] = {
        L"Dear team,",
        L"",
        L"Hello wrold",
        L"",
        L"Thanks for the quick turnaround on the release notes. I went",
        L"through the draft this morning and only have a couple of small",
        L"comments. The section about the new keyboard shortcuts reads",
        L"well, but the example under \"Getting started\" still mentions",
        L"the old settings page. Could we also add a short note about how",
        L"corrections work when you keep typing? People asked about it",
        L"twice in the beta forum. Otherwise this looks ready to ship.",
        L"",
        L"Best,",
        L"Sam",
    };
    int y = 24;
    for (const auto* line : lines) {
        TextOutW(dc, 28, y, line, static_cast<int>(wcslen(line)));
        y += 28;
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (g_duskBackdrop) {
            PaintDusk(dc, client);
        } else if (g_paperBackdrop) {
            PaintPaper(dc, client);
        } else if (g_documentBackdrop) {
            PaintDocument(dc, client);
        } else {
            PaintStripes(dc, client);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateBackdropWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = &BackdropProc;
    wc.lpszClassName = L"TekitoPreviewBackdrop";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                L"TekitoPreviewBackdrop", L"", WS_POPUP, 160, 200, 620, 460,
                                nullptr, nullptr, instance, nullptr);
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

bool SaveScreenRectToBmp(RECT rect, const wchar_t* path) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);
    BitBlt(memDc, 0, 0, width, height, screenDc, rect.left, rect.top, SRCCOPY | CAPTUREBLT);

    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    BITMAPINFO bmi{};
    bmi.bmiHeader = infoHeader;
    GetDIBits(memDc, bitmap, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());

    FILE* file = nullptr;
    _wfopen_s(&file, path, L"wb");
    bool ok = false;
    if (file) {
        fwrite(&fileHeader, sizeof(fileHeader), 1, file);
        fwrite(&infoHeader, sizeof(infoHeader), 1, file);
        fwrite(pixels.data(), pixels.size(), 1, file);
        fclose(file);
        ok = true;
    }

    SelectObject(memDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return ok;
}

tekito::Candidate MakeCandidate(const wchar_t* text, tekito::SemanticLabel label, std::uint32_t id,
                                bool original = false) {
    tekito::Candidate candidate;
    candidate.text = text;
    candidate.label = label;
    candidate.isOriginal = original;
    candidate.id = id;
    return candidate;
}

std::vector<tekito::Candidate> SampleCandidates(bool longList) {
    using tekito::SemanticLabel;
    std::vector<tekito::Candidate> candidates{
        MakeCandidate(L"wrold", SemanticLabel::None, 1, true),
        MakeCandidate(L"world", SemanticLabel::None, 2),
        MakeCandidate(L"would", SemanticLabel::None, 3),
        MakeCandidate(L"\U0001F30D", SemanticLabel::Emoji, 4),
        MakeCandidate(L"worldwide", SemanticLabel::None, 5),
    };
    if (longList) {
        const wchar_t* more[] = {L"worldly", L"word", L"worlds", L"wold", L"wield", L"weld", L"wroth"};
        std::uint32_t id = 6;
        for (const auto* text : more) {
            candidates.push_back(MakeCandidate(text, SemanticLabel::None, id++));
        }
    }
    return candidates;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* outputPath = argc > 1 ? argv[1] : L"candidate_window_preview.bmp";
    bool longList = false;
    bool glide = false;
    bool click = false;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"doc") == 0) g_documentBackdrop = true;
        if (wcscmp(argv[i], L"dusk") == 0) g_duskBackdrop = true;
        if (wcscmp(argv[i], L"paper") == 0) g_paperBackdrop = true;
        if (wcscmp(argv[i], L"long") == 0) longList = true;
        if (wcscmp(argv[i], L"glide") == 0) glide = true;
        if (wcscmp(argv[i], L"click") == 0) click = true;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND backdrop = CreateBackdropWindow(instance);

    tekito::tsf::CandidateWindow window;
    const DWORD mainThread = GetCurrentThreadId();
    if (!window.Initialize(instance, [mainThread](std::size_t index) {
            wprintf(L"Selected index %zu on %ls thread\n", index,
                    GetCurrentThreadId() == mainThread ? L"the host" : L"another");
        })) {
        fwprintf(stderr, L"Initialize failed\n");
        return 1;
    }

    wchar_t style[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_STYLE", style, 16) > 0 && wcscmp(style, L"simple") == 0) {
        window.SetStyle(1);
    }
    const auto candidates = SampleCandidates(longList);
    // Caret under "wrold" on the document's third line.
    RECT caret{232, 280, 234, 302};
    LARGE_INTEGER frequency{};
    LARGE_INTEGER shown{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&shown);
    window.Show(caret, candidates, 1, 0, 5);
    // Time until the UI thread has built its windows and presented the first
    // frame (composition init, device creation and effect compilation).
    for (int spin = 0; spin < 5000; ++spin) {
        HWND panel = FindWindowW(L"TekitoCandidateWindow", nullptr);
        if (panel && IsWindowVisible(panel)) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            wprintf(L"First frame after %.1f ms\n",
                    1000.0 * static_cast<double>(now.QuadPart - shown.QuadPart) /
                        static_cast<double>(frequency.QuadPart));
            break;
        }
        Sleep(1);
    }
    const auto pump = [](ULONGLONG milliseconds) {
        MSG msg;
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < milliseconds) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(1);
        }
    };
    pump(1500);

    HWND target = FindWindowW(L"TekitoCandidateWindow", nullptr);
    if (!target) {
        fwprintf(stderr, L"Candidate window not found\n");
        return 1;
    }
    if (click) {
        // Row 4 (index 3): padding 6 + 3 rows of (36 + 2) + half a row, in DIPs.
        const UINT dpi = GetDpiForWindow(target);
        const int y = MulDiv(6 + 3 * 38 + 18, static_cast<int>(dpi), 96);
        const int x = MulDiv(60, static_cast<int>(dpi), 96);
        PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        pump(300);
    }
    if (glide) {
        window.Show(caret, candidates, 3, 0, 5);
        pump(35);
    }
    RECT rect{};
    GetWindowRect(target, &rect);
    InflateRect(&rect, kCaptureMargin, kCaptureMargin);
    if (!SaveScreenRectToBmp(rect, outputPath)) {
        fwprintf(stderr, L"Screenshot failed\n");
        return 1;
    }
    wprintf(L"Saved %ls\n", outputPath);
    window.Hide();
    if (backdrop) DestroyWindow(backdrop);
    return 0;
}
