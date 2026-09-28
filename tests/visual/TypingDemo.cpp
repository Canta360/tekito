// Records the README typing demos: a short line typed key by key into a
// plain page, with the real engine producing the candidate list and the real
// typing state machine deciding what Space and punctuation do. Each step is
// captured as a BMP frame; scripts/make-demo-animation.py assembles them.
//
//   tekito_typing_demo.exe <output folder> [--japanese]
//
// --japanese types romaji into the Japanese composer instead, converting with
// Space and stepping into the list for a slip.
//
// Needs the Data Packs (TEKITO_DATA_PACK_DIR) and a visible desktop.
#include "Core/ConversionEngine.h"
#include "Core/InputStateMachine.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/RomajiTable.h"
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
// Room for nine candidates under the line.
constexpr int kJapaneseHeight = 450;
constexpr int kMargin = 28;
constexpr int kLineTop = 34;

struct Page {
    std::wstring committed;    // text before the word being typed
    std::wstring composition;  // the word being typed, underlined
    // Japanese: the composition in phrases; converted ones are underlined
    // solid, the focused one thicker.
    std::vector<tekito::japanese::PreeditSegment> segments;
    bool caret{true};
};

Page g_page;
HFONT g_font = nullptr;

int TextWidth(HDC dc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

// The text of the segments before the focused one, and the focused one (the
// whole composition when none is focused).
std::pair<std::wstring, std::wstring> FocusedSplit() {
    std::wstring before;
    for (const auto& segment : g_page.segments) {
        if (segment.focused) return {before, segment.text};
        before += segment.text;
    }
    return {L"", g_page.composition};
}

// Screen rectangle of the composition (the focused phrase in Japanese),
// where the candidate list anchors.
RECT CompositionRect(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, g_font);
    const auto [leading, focused] = FocusedSplit();
    SIZE before{TextWidth(dc, g_page.committed + leading), 0};
    SIZE word{TextWidth(dc, focused), 0};
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
    if (!g_page.segments.empty()) {
        int x = kMargin + before.cx;
        for (const auto& segment : g_page.segments) {
            const int width = TextWidth(dc, segment.text);
            HPEN pen = CreatePen(segment.converted ? PS_SOLID : PS_DOT, segment.focused ? 2 : 1, RGB(28, 30, 34));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, x + 1, baseline, nullptr);
            LineTo(dc, x + width - 1, baseline);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            x += width;
        }
    } else if (!g_page.composition.empty()) {
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

std::filesystem::path DataRoot() {
    wchar_t value[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"TEKITO_DATA_PACK_DIR", value, MAX_PATH) > 0) return value;
    return L"data";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::filesystem::path outputFolder = argc > 1 ? argv[1] : L"typing-demo";
    const bool japanese = argc > 2 && std::wstring_view(argv[2]) == L"--japanese";
    std::filesystem::create_directories(outputFolder);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    g_font = CreateFontW(japanese ? -21 : -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                         japanese ? L"Yu Gothic UI" : L"Segoe UI");
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = &PageProc;
    windowClass.lpszClassName = L"TekitoTypingDemoPage";
    RegisterClassExW(&windowClass);
    HWND page = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                windowClass.lpszClassName, L"", WS_POPUP, 160, 200, kWidth, japanese ? kJapaneseHeight : kHeight,
                                nullptr, nullptr, instance, nullptr);
    ShowWindow(page, SW_SHOWNOACTIVATE);

    tekito::tsf::CandidateWindow window;
    if (!window.Initialize(instance, [](std::size_t) {})) return 1;
    // TEKITO_PREVIEW_LANGUAGE=ja records the demo with the Japanese tags.
    wchar_t language[8]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_LANGUAGE", language, 8) > 0 && wcscmp(language, L"ja") == 0) {
        window.SetJapanese(true);
    }

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

    if (japanese) {
        namespace ja = tekito::japanese;
        const auto root = DataRoot();
        ja::JapaneseDictionary dictionary;
        ja::ConnectionMatrix matrix;
        if (!dictionary.Open(root / L"japanese-core" / L"dictionary.bin") ||
            !matrix.Open(root / L"japanese-core" / L"connection.bin")) {
            return 1;
        }
        const auto table = ja::RomajiTable::Load(root / L"japanese-romaji");
        if (!table) return 1;
        ja::JapaneseConverter converter(dictionary, matrix);
        ja::LanguageModel model;
        if (model.Open(root / L"japanese-lm")) converter.SetLanguageModel(&model);
        const ja::KeyConverter keys(dictionary, matrix, converter, *table);
        ja::Loanwords loanwords;
        ja::MeaningDictionary meanings;
        meanings.Open(root);
        ja::JapaneseComposer composer(table.get());
        composer.SetConverter(&converter);
        composer.SetKeyConverter(&keys);
        if (loanwords.Open(root / L"japanese-loanwords")) composer.SetLoanwords(&loanwords);
        window.SetJapanese(true);

        constexpr std::size_t kPage = 9;
        const auto render = [&] {
            g_page.segments = composer.Segments();
            g_page.composition = composer.Preedit();
            const auto* candidates = composer.FocusedCandidates();
            if (!composer.IsCandidateListOpen() || !candidates || candidates->empty()) {
                window.Hide();
                return;
            }
            const std::size_t selected = std::min(composer.FocusedSelection(), candidates->size() - 1);
            const std::size_t first = selected / kPage * kPage;
            std::vector<tekito::Candidate> rows;
            for (std::size_t i = 0; i < candidates->size(); ++i) {
                tekito::Candidate row;
                row.text = (*candidates)[i].text;
                row.id = static_cast<std::uint32_t>(i % kPage + 1);
                if ((*candidates)[i].slip || (*candidates)[i].spellingCorrection) {
                    row.label = tekito::SemanticLabel::Suggestion;
                }
                if (i >= first && i < first + kPage) {
                    row.hasMeaning = meanings.Lookup(row.text, composer.FocusedReading()).has_value();
                }
                rows.push_back(std::move(row));
            }
            tekito::tsf::CandidateDetail detail;
            if (const auto meaning = meanings.Lookup((*candidates)[selected].text, composer.FocusedReading())) {
                detail = {meaning->headword, meaning->senses};
            }
            window.Show(CompositionRect(page), rows, selected, first, std::min(kPage, rows.size() - first), detail);
        };
        const auto commit = [&](int milliseconds) {
            window.Hide();
            g_page.committed += composer.Commit();
            g_page.segments.clear();
            g_page.composition.clear();
            capture(milliseconds);
        };
        const auto type = [&](std::wstring_view keysTyped) {
            for (std::size_t i = 0; i < keysTyped.size(); ++i) {
                composer.Insert(keysTyped[i]);
                render();
                capture(i + 1 == keysTyped.size() ? 500 : 110);
            }
        };
        const auto space = [&](int milliseconds) {
            composer.Convert();
            render();
            wprintf(L"Space -> [%ls]\n", composer.Preedit().c_str());
            capture(milliseconds);
        };

        capture(700);
        // A slip: "hahimemasite" for はじめまして. The first conversion is
        // what was typed; the second Space opens the list, where what was
        // meant waits, marked.
        type(L"hahimemasite");
        space(900);
        space(1100);
        const auto* candidates = composer.FocusedCandidates();
        std::size_t slip = 0;
        while (candidates && slip < candidates->size() && !(*candidates)[slip].slip) ++slip;
        if (candidates && slip < candidates->size()) {
            while (composer.FocusedSelection() < slip) {
                composer.NextCandidate();
                render();
                capture(composer.FocusedSelection() == slip ? 1500 : 450);
            }
        }
        commit(500);
        composer.Insert(L'.');
        commit(500);
        type(L"kyouhaiitenkidesune.");
        space(1100);
        commit(2600);
        window.Hide();
        DestroyWindow(page);
        return 0;
    }

    tekito::UserDictionary dictionary;
    const auto engine = tekito::CreateDefaultConversionEngine(dictionary);
    if (!engine) return 1;
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
