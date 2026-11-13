// Records the README typing demos: a short line typed key by key into a
// plain page, with the real engine producing the candidate list and the real
// typing state machine deciding what Space and punctuation do. Each step is
// captured as a BMP frame; scripts/make-demo-animation.py assembles them.
//
//   tekito_typing_demo.exe <output folder> [--japanese | --live | --mix | --mix-live]
//
// --japanese types a Japanese sentence with slips in it in Japanese mode, as
// the input method sets it up with the default settings: one Space puts them
// right. --live types it with live conversion on. --mix types four lines in
// Mix, a Space after each part: the same Japanese sentence, the English
// demo's sentence, Japanese with English, and that typed without spaces.
// --mix-live types that last line with Mix's live conversion on.
// TEKITO_PREVIEW_LANGUAGE=ja shows the Japanese tags.
//
// Needs the Data Packs (TEKITO_DATA_PACK_DIR) and a visible desktop.
#include "Core/ConversionEngine.h"
#include "Core/DataPackPath.h"
#include "Core/EnglishSpelling.h"
#include "Core/InputStateMachine.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/UserDictionary.h"
#include "Tsf/CandidateWindow.h"
#include "UserData/JapaneseSettings.h"
#include "UserData/UserSettings.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

// The page covers the screen, so the list and its meaning pane never run
// past it; make-demo-animation.py --trim crops the frames to what was drawn.
constexpr int kMargin = 28;
constexpr int kLineTop = 34;
constexpr int kLineGap = 14;  // between lines, beyond the font's height

struct Page {
    std::wstring committed;    // text before the word being typed; '\n' starts a line
    std::wstring composition;  // the word being typed, underlined
    // Japanese: the composition in phrases; converted ones are underlined
    // solid, the focused one thicker.
    std::vector<tekito::japanese::PreeditSegment> segments;
    // Mix: underlines in its orange, and the chunks held until Enter on
    // its orange marker, as the text service's display attributes draw them.
    bool mix{false};
    bool marker{false};
    bool caret{true};
};

constexpr COLORREF kInk = RGB(28, 30, 34);
constexpr COLORREF kMixOrange = RGB(245, 150, 25);
constexpr COLORREF kMixMarker = RGB(252, 212, 160);
constexpr COLORREF kMixHeld = RGB(190, 190, 190);
constexpr COLORREF kAutoBlue = RGB(0, 103, 248);

Page g_page;
HFONT g_font = nullptr;

int TextWidth(HDC dc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

// The committed text in lines; the last is the one being typed on.
std::vector<std::wstring> CommittedLines() {
    std::vector<std::wstring> lines{L""};
    for (const wchar_t ch : g_page.committed) {
        if (ch == L'\n') {
            lines.emplace_back();
        } else {
            lines.back() += ch;
        }
    }
    return lines;
}

// The top of the line being typed on.
int CurrentLineTop(HDC dc) {
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    return kLineTop + static_cast<int>(CommittedLines().size() - 1) * (metrics.tmHeight + kLineGap);
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
    SIZE before{TextWidth(dc, CommittedLines().back() + leading), 0};
    SIZE word{TextWidth(dc, focused), 0};
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int top = CurrentLineTop(dc);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    POINT topLeft{kMargin + before.cx, top};
    POINT bottomRight{kMargin + before.cx + word.cx, top + metrics.tmHeight};
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
    SetTextColor(dc, kInk);
    HGDIOBJ old = SelectObject(dc, g_font);
    // The composition as the text service writes it: its segments' text.
    std::wstring composition = g_page.composition;
    if (!g_page.segments.empty()) {
        composition.clear();
        for (const auto& segment : g_page.segments) composition += segment.text;
    }
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    // The lines before the one being typed on.
    const auto lines = CommittedLines();
    for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
        TextOutW(dc, kMargin, kLineTop + static_cast<int>(i) * (metrics.tmHeight + kLineGap), lines[i].c_str(),
                 static_cast<int>(lines[i].size()));
    }
    const int lineTop = CurrentLineTop(dc);
    const std::wstring line = lines.back() + composition;

    SIZE before{};
    SIZE all{};
    GetTextExtentPoint32W(dc, lines.back().c_str(), static_cast<int>(lines.back().size()), &before);
    GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &all);
    // Mix's marker behind the chunks held until Enter.
    if (g_page.mix) {
        int x = kMargin + before.cx;
        for (const auto& segment : g_page.segments) {
            const int width = TextWidth(dc, segment.text);
            if (segment.settled && g_page.marker) {
                RECT marker{x, lineTop, x + width, lineTop + metrics.tmHeight};
                HBRUSH brush = CreateSolidBrush(kMixMarker);
                FillRect(dc, &marker, brush);
                DeleteObject(brush);
            }
            x += width;
        }
    }
    TextOutW(dc, kMargin, lineTop, line.c_str(), static_cast<int>(line.size()));

    const int baseline = lineTop + metrics.tmHeight + 1;
    if (!g_page.segments.empty()) {
        int x = kMargin + before.cx;
        for (const auto& segment : g_page.segments) {
            const int width = TextWidth(dc, segment.text);
            if (g_page.mix && segment.settled && g_page.marker) {
                x += width;
                continue;
            }
            // Without the marker, the held sentence is underlined in light grey.
            HPEN pen = CreatePen(segment.converted || segment.settled ? PS_SOLID : PS_DOT, segment.focused ? 2 : 1,
                                 segment.settled ? kMixHeld : g_page.mix ? kMixOrange : kInk);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, x + 1, baseline, nullptr);
            LineTo(dc, x + width - 1, baseline);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            x += width;
        }
    } else if (!g_page.composition.empty()) {
        // Dotted underline, in English Auto's blue as the text service has it.
        HPEN pen = CreatePen(PS_DOT, 1, kAutoBlue);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, kMargin + before.cx, baseline, nullptr);
        LineTo(dc, kMargin + all.cx, baseline);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    if (g_page.caret) {
        RECT caret{kMargin + all.cx + 1, lineTop, kMargin + all.cx + 2, lineTop + metrics.tmHeight};
        HBRUSH ink = CreateSolidBrush(kInk);
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
    const bool mixLive = argc > 2 && std::wstring_view(argv[2]) == L"--mix-live";
    const bool mix = mixLive || (argc > 2 && std::wstring_view(argv[2]) == L"--mix");
    const bool live = argc > 2 && std::wstring_view(argv[2]) == L"--live";
    const bool japanese = mix || live || (argc > 2 && std::wstring_view(argv[2]) == L"--japanese");
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
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND page = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                windowClass.lpszClassName, L"", WS_POPUP, work.left, work.top,
                                work.right - work.left, work.bottom - work.top,
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
    int clipped = 0;
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
        // The list (with its shadow) must sit inside the page, or the frame
        // shows it cut off.
        RECT list{};
        HWND panel = FindWindowW(L"TekitoCandidateWindow", nullptr);
        if (panel && IsWindowVisible(panel) && GetWindowRect(panel, &list) &&
            (list.left < rect.left || list.top < rect.top || list.right > rect.right || list.bottom > rect.bottom)) {
            fprintf(stderr, "%s: the list runs past the page\n", name);
            ++clipped;
        }
    };

    if (japanese) {
        namespace ja = tekito::japanese;
        const auto root = DataRoot();
        // As the input method sets it up: the installed data, the default
        // settings, Mix's when it is Mix.
        ja::JapaneseComposer composer(ja::ProcessRomajiTable());
        ja::AttachJapaneseData(composer);
        if (!composer.Converter()) return 1;
        tekito::userdata::UserSettings settings;
        settings.japaneseLiveConversion = live;
        settings.mixLiveConversion = mixLive;
        tekito::userdata::ApplyJapaneseSettings(composer, settings, nullptr, mix);
        ja::AttachMixedTyping(composer, mix);
        // English Auto's spelling for Mix's English, as the text service gives it.
        tekito::UserDictionary englishWords;
        const auto englishEngine = tekito::CreateDefaultConversionEngine(englishWords);
        std::unique_ptr<tekito::EnglishSpelling> englishSpelling;
        if (englishEngine) {
            englishSpelling = std::make_unique<tekito::EnglishSpelling>(*englishEngine);
            composer.SetEnglishSpelling(englishSpelling.get());
        }
        ja::MeaningDictionary meanings;
        meanings.Open(root);
        g_page.mix = mix;
        g_page.marker = tekito::userdata::UserSettings{}.mixSettledMarker;

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
                // A line after the phrases written as a whole, as the text service draws it.
                row.groupStart = i > 0 && (*candidates)[i - 1].chunk && !(*candidates)[i].chunk;
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
            // The app's text before the caret, as the text service reads it.
            composer.SetPrecedingText(g_page.committed);
            capture(milliseconds);
        };
        // As the text service types a key: after a conversion, the key
        // commits it first.
        const auto insert = [&](wchar_t key) {
            if (composer.CommitsBeforeTyping()) {
                g_page.committed += composer.Commit();
                composer.SetPrecedingText(g_page.committed);
            }
            composer.Insert(key);
        };
        const auto type = [&](std::wstring_view keysTyped, int perKey = 110) {
            for (std::size_t i = 0; i < keysTyped.size(); ++i) {
                insert(keysTyped[i]);
                render();
                capture(i + 1 == keysTyped.size() ? 500 : perKey);
            }
        };
        const auto space = [&](int milliseconds) {
            composer.Convert();
            render();
            wprintf(L"Space -> [%ls]\n", composer.Preedit().c_str());
            capture(milliseconds);
        };


        capture(700);
        // Enter on an empty composition: the app starts a new line.
        const auto newLine = [&](int milliseconds) {
            g_page.committed += L'\n';
            composer.SetPrecedingText(g_page.committed);
            capture(milliseconds);
        };
        // Parts typed with a Space after each (Mix), then Enter.
        const auto parts = [&](std::initializer_list<const wchar_t*> typed, int perKey) {
            for (const wchar_t* part : typed) {
                type(part, perKey);
                space(450);
            }
        };
        // The same lines with slips in them as the English and Japanese
        // demos: a key left out (nattermasu), one not converted (そうしん),
        // a neighbor key (simasira); English slips; Japanese with English.
        // A pause in live conversion: the text converted again as a whole.
        const auto pause = [&](int milliseconds) {
            composer.RefineLive();
            render();
            capture(milliseconds);
        };
        if (mixLive) {
            // Japanese with English in Mix with live conversion on, typed
            // without spaces: converted and split as it is typed.
            parts({L"atarasiiapinosiyousyowogithubniappusitanode,urlwokakuninsitekudasai."}, 80);
            commit(3000);
        } else if (mix) {
            // Japanese, English, the two mixed, and the same mixed sentence
            // typed without spaces (spelled right: English run into Japanese
            // is split off only when it is), all in Mix.
            parts({L"osewaninatterimasu,", L"honjitunokaiginosiryouwome-rudesousinsimasira."}, 55);
            commit(1300);
            newLine(300);
            parts({L"thank", L"you", L"for", L"your", L"quivk", L"reoly,", L"i", L"recived", L"the", L"flie", L"and",
                   L"will", L"check", L"it", L"tomorow."}, 55);
            commit(1300);
            newLine(300);
            parts({L"atarasii", L"api", L"no", L"siyousyowo", L"gothub", L"ni", L"appusitanode,", L"url", L"wo",
                   L"kakuninsitekudadai."}, 55);
            commit(1300);
            newLine(300);
            parts({L"atarasiiapinosiyousyowogithubniappusitanode,urlwokakuninsitekudasai."}, 55);
            commit(3000);
        } else if (live) {
            // The same sentence in live conversion: converted as it is typed,
            // and again as a whole when typing pauses (なってリマス becomes
            // なっております); Enter takes what is shown.
            type(L"osewaninatterimasu,", 90);
            pause(1500);
            commit(500);
            type(L"honjitunokaiginosiryouwome-rudesousinsimasira.", 90);
            pause(1800);
            commit(2800);
        } else {
            // お世話になっております、本日の会議の資料をメールで送信しました。
            // in two goes, as people convert it.
            type(L"osewaninatterimasu,", 90);
            space(1500);
            commit(500);
            type(L"honjitunokaiginosiryouwome-rudesousinsimasira.", 90);
            space(2200);
            commit(2800);
        }
        window.Hide();
        DestroyWindow(page);
        return clipped ? 3 : 0;
    }

    tekito::UserDictionary dictionary;
    const auto engine = tekito::CreateDefaultConversionEngine(dictionary);
    if (!engine) return 1;
    // Rows with a meaning carry the book sign, and the highlighted one's
    // meaning shows beside the list, as the TSF does.
    tekito::japanese::MeaningDictionary meanings;
    meanings.Open(DataRoot());
    const auto show = [&](const tekito::InputStateMachine& state) {
        auto rows = state.Candidates();
        for (std::size_t i = state.PageStart(); i < rows.size() && i < state.PageStart() + state.VisibleCount(); ++i) {
            rows[i].hasMeaning = meanings.Lookup(rows[i].text).has_value();
        }
        tekito::tsf::CandidateDetail detail;
        if (state.SelectedIndex() < rows.size()) {
            if (const auto meaning = meanings.Lookup(rows[state.SelectedIndex()].text)) {
                detail = {meaning->headword, meaning->senses};
            }
        }
        window.Show(CompositionRect(page), rows, state.SelectedIndex(), state.PageStart(), state.VisibleCount(),
                    detail);
    };

    capture(900);
    // Each word with a Space after it, or the comma or period that ends it
    // (and the Space after a comma, which the app types).
    const std::wstring words[] = {L"Thank", L"you", L"for", L"your", L"quivk", L"reoly,", L"I", L"recived", L"the",
                                  L"flie", L"and", L"will", L"check", L"it", L"tomorow."};
    for (std::size_t w = 0; w < std::size(words); ++w) {
        std::wstring word = words[w];
        wchar_t mark = 0;
        if (word.back() == L',' || word.back() == L'.') {
            mark = word.back();
            word.pop_back();
        }
        tekito::InputStateMachine state;
        for (std::size_t length = 1; length <= word.size(); ++length) {
            g_page.composition = word.substr(0, length);
            tekito::ConversionRequest request;
            request.rawText = g_page.composition;
            request.context.precedingText = g_page.committed;
            state.BeginOrUpdate(g_page.composition, engine->Convert(request).candidates);
            show(state);
            capture(length == word.size() ? 600 : 130);
        }
        std::wstring listed;
        for (std::size_t c = 0; c < state.Candidates().size() && c < 5; ++c) listed += L" " + state.Candidates()[c].text;
        const auto action = mark == L'.'   ? state.OnPunctuation(L'.', tekito::PunctuationRole::SentenceTerminal)
                            : mark == L',' ? state.OnPunctuation(L',', tekito::PunctuationRole::ClauseSeparator)
                                           : state.OnSpace();
        wprintf(L"%ls -> [%ls] (list:%ls)\n", words[w].c_str(), action.text.c_str(), listed.c_str());
        window.Hide();
        g_page.committed += action.text;
        if (mark == L',') g_page.committed += L' ';
        g_page.composition.clear();
        capture(mark == L'.' ? 2600 : 350);
    }
    window.Hide();
    DestroyWindow(page);
    return clipped ? 3 : 0;
}
