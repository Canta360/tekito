// A window to try Japanese typing in without registering the input method:
// keys go through the same code the text service uses (JapaneseKeyPressFor,
// japanese::TranslateKey and ApplyKey, CandidateListFor), with the real
// candidate window, the installed Data Packs and your settings, user words
// and learning, which it reads but never writes back.
//
//   tekito_testbed.exe [--defaults]
//
// --defaults types with the default settings and no user words or learning.
// TEKITO_DATA_PACK_DIR picks other Data Packs (the repository's data folder,
// say); otherwise the installed ones are used. Ctrl+L clears the page; F12
// turns live conversion on or off.
//
// What it cannot show is how applications treat a composition: caret
// placement, Word or a browser. Check those with the input method registered.
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/JapaneseKeys.h"
#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/PostalCodes.h"
#include "Tsf/CandidateWindow.h"
#include "Tsf/JapaneseKeyPress.h"
#include "UserData/JapaneseSettings.h"
#include "UserData/SqliteUserDictionaryRepository.h"
#include "UserData/UiLanguage.h"
#include "UserData/UserSettings.h"

#include <windows.h>
#include <imm.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ja = tekito::japanese;

constexpr int kMargin = 24;

struct Testbed {
    tekito::userdata::UserSettings settings;
    bool userData{false};
    ja::JapaneseLearningStore learning;
    ja::JapaneseUserDictionary userWords;
    std::shared_ptr<const ja::PostalCodes> postalCodes;
    ja::JapaneseComposer composer;
    tekito::tsf::CandidateWindow candidates;
    // What was typed and committed so far; the composition follows it.
    std::wstring text;
    HWND window{nullptr};
    HFONT font{nullptr};
    HFONT small{nullptr};
    UINT fontDpi{0};
    // Where the caret and the focused phrase were drawn, in screen
    // coordinates, for the candidate window.
    RECT caretRect{};
    RECT focusRect{};
    bool hasFocusRect{false};
};

Testbed* g_bed = nullptr;

// One character laid out on the page, and how it is underlined.
enum class Style { Committed, Input, Converted, Focused };
struct Cell {
    wchar_t ch;
    Style style;
    RECT rect;
};

void LoadUserData(Testbed& bed, bool defaults) {
    if (defaults) return;
    const auto path = tekito::userdata::DefaultUserDatabasePath();
    if (path.empty()) return;
    tekito::userdata::SqliteUserDictionaryRepository repository(path);
    if (!repository.Open()) return;
    bed.userData = repository.LoadSettings(bed.settings);
    (void)repository.LoadJapaneseLearning(bed.learning);
    std::vector<ja::UserWord> words;
    if (repository.LoadJapaneseUserWords(words)) {
        if (const auto* parts = ja::ProcessUserParts()) bed.userWords.Set(words, *parts);
    }
}

void SetUpComposer(Testbed& bed) {
    bed.composer.SetTable(ja::ProcessRomajiTable());
    ja::AttachJapaneseData(bed.composer);
    // Learning is kept for this session only: nothing is written back.
    tekito::userdata::ApplyJapaneseSettings(bed.composer, bed.settings, &bed.learning);
    bed.composer.SetUserDictionary(bed.userWords.Empty() ? nullptr : &bed.userWords);
    bed.postalCodes = ja::ProcessPostalCodes();
    bed.composer.SetPostalCodes(bed.postalCodes.get());
}

void EnsureFonts(Testbed& bed) {
    const UINT dpi = GetDpiForWindow(bed.window);
    if (bed.font && dpi == bed.fontDpi) return;
    if (bed.font) DeleteObject(bed.font);
    if (bed.small) DeleteObject(bed.small);
    bed.fontDpi = dpi;
    bed.font = CreateFontW(-MulDiv(22, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Yu Gothic UI");
    bed.small = CreateFontW(-MulDiv(13, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
}

int Scale(const Testbed& bed, int value) { return MulDiv(value, static_cast<int>(bed.fontDpi), 96); }

std::wstring StatusLine(const Testbed& bed) {
    std::wstring line = L"TEKITO Testbed  |  Japanese  |  ";
    line += bed.userData ? L"your settings, words and learning (read only)" : L"default settings";
    line += L"  |  data: " + tekito::ExternalLexiconProvider::DataPackRoot().wstring();
    const auto missing = ja::MissingJapaneseData();
    if (!missing.empty()) {
        line += L"  |  missing:";
        for (const auto part : missing) line += L" " + std::wstring(part);
    }
    line += bed.composer.LiveConversion() ? L"  |  live conversion on (F12)" : L"  |  live conversion off (F12)";
    line += L"  |  Ctrl+L clears";
    return line;
}

// Lays out the committed text and the composition, wrapping at `width`.
std::vector<Cell> Layout(const Testbed& bed, HDC dc, int top, int width, std::size_t& caretCell) {
    std::vector<std::pair<wchar_t, Style>> chars;
    for (const wchar_t ch : bed.text) chars.push_back({ch, Style::Committed});
    const std::size_t compositionStart = chars.size();
    for (const auto& segment : bed.composer.Segments()) {
        const Style style = !segment.converted ? Style::Input : segment.focused ? Style::Focused : Style::Converted;
        for (const wchar_t ch : segment.text) chars.push_back({ch, style});
    }
    caretCell = bed.composer.IsComposing() ? compositionStart + bed.composer.CaretOffset() : chars.size();

    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight + metrics.tmExternalLeading + Scale(bed, 10);
    std::vector<Cell> cells;
    int x = kMargin;
    int y = top;
    for (const auto& [ch, style] : chars) {
        if (ch == L'\n') {
            cells.push_back({ch, style, {x, y, x, y + metrics.tmHeight}});
            x = kMargin;
            y += lineHeight;
            continue;
        }
        SIZE size{};
        GetTextExtentPoint32W(dc, &ch, 1, &size);
        if (x + size.cx > width - kMargin && x > kMargin) {
            x = kMargin;
            y += lineHeight;
        }
        cells.push_back({ch, style, {x, y, x + size.cx, y + metrics.tmHeight}});
        x += size.cx;
    }
    // Where the caret goes after the last character.
    cells.push_back({0, Style::Committed, {x, y, x, y + metrics.tmHeight}});
    return cells;
}

void Paint(Testbed& bed, HDC target) {
    EnsureFonts(bed);
    RECT client{};
    GetClientRect(bed.window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);

    // The status line.
    HGDIOBJ oldFont = SelectObject(dc, bed.small);
    SetTextColor(dc, RGB(96, 96, 96));
    RECT status{kMargin, Scale(bed, 8), client.right - kMargin, Scale(bed, 30)};
    const auto line = StatusLine(bed);
    DrawTextW(dc, line.c_str(), static_cast<int>(line.size()), &status, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectObject(dc, bed.font);
    SetTextColor(dc, RGB(20, 20, 20));
    std::size_t caretCell = 0;
    const auto cells = Layout(bed, dc, Scale(bed, 44), client.right, caretCell);
    HPEN thin = CreatePen(PS_SOLID, Scale(bed, 1), RGB(40, 40, 40));
    HPEN thick = CreatePen(PS_SOLID, Scale(bed, 2), RGB(40, 40, 40));
    HPEN dotted = CreatePen(PS_DOT, 1, RGB(90, 90, 90));
    bed.hasFocusRect = false;
    RECT focus{};
    for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
        const auto& cell = cells[i];
        if (cell.ch == L'\n') continue;
        TextOutW(dc, cell.rect.left, cell.rect.top, &cell.ch, 1);
        if (cell.style == Style::Committed) continue;
        HPEN pen = cell.style == Style::Focused ? thick : cell.style == Style::Converted ? thin : dotted;
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int y = cell.rect.bottom + Scale(bed, 1);
        // A gap between phrases, as editors draw them.
        MoveToEx(dc, cell.rect.left + 1, y, nullptr);
        LineTo(dc, cell.rect.right - 1, y);
        SelectObject(dc, oldPen);
        if (cell.style == Style::Focused) {
            if (!bed.hasFocusRect) focus = cell.rect;
            UnionRect(&focus, &focus, &cell.rect);
            bed.hasFocusRect = true;
        }
    }
    // The caret.
    const RECT caret = cells[std::min(caretCell, cells.size() - 1)].rect;
    RECT bar{caret.left, caret.top, caret.left + Scale(bed, 2), caret.bottom};
    FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    // Screen coordinates for the candidate window.
    POINT corners[4] = {{caret.left, caret.top}, {caret.right, caret.bottom}, {focus.left, focus.top},
                        {focus.right, focus.bottom}};
    MapWindowPoints(bed.window, nullptr, corners, 4);
    bed.caretRect = {corners[0].x, corners[0].y, corners[1].x + 1, corners[1].y};
    bed.focusRect = {corners[2].x, corners[2].y, corners[3].x, corners[3].y};

    SelectObject(dc, oldFont);
    DeleteObject(thin);
    DeleteObject(thick);
    DeleteObject(dotted);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

tekito::tsf::CandidateDetail MeaningFor(const Testbed& bed, const std::wstring& text, const std::wstring& reading) {
    if (!bed.settings.meaningsEnabled || text.empty()) return {};
    const auto meaning = ja::ProcessMeanings().Lookup(text, reading);
    if (!meaning) return {};
    return {meaning->headword, meaning->senses};
}

// The candidate window, as the text service shows it (ShowJapaneseCandidates).
void ShowCandidates(Testbed& bed) {
    using Kind = ja::CandidateList::Kind;
    const auto list = ja::CandidateListFor(bed.composer, tekito::userdata::JapanesePageSize(bed.settings));
    if (list.kind == Kind::None || !bed.settings.candidateWindowEnabled) {
        bed.candidates.Hide();
        return;
    }
    std::vector<tekito::Candidate> rows;
    for (std::size_t i = 0; i < list.rows.size(); ++i) {
        tekito::Candidate row;
        row.text = list.rows[i].text;
        row.id = list.rows[i].number;
        if (list.rows[i].suggestion) row.label = tekito::SemanticLabel::Suggestion;
        row.hasMeaning = bed.settings.meaningsEnabled && i >= list.pageStart && i < list.pageStart + list.count &&
                         ja::ProcessMeanings().Lookup(list.rows[i].text, list.rows[i].reading).has_value();
        rows.push_back(std::move(row));
    }
    const auto& selected = list.selected;
    const auto detail = selected && *selected < list.rows.size()
                            ? MeaningFor(bed, list.rows[*selected].text, list.rows[*selected].reading)
                            : tekito::tsf::CandidateDetail{};
    const RECT anchor = list.kind == Kind::Candidates && bed.hasFocusRect ? bed.focusRect : bed.caretRect;
    bed.candidates.SetStyle(bed.settings.candidateWindowStyle);
    bed.candidates.SetJapanese(tekito::userdata::UseJapaneseUi(bed.settings.uiLanguage));
    bed.candidates.Show(anchor, rows, selected ? *selected : tekito::tsf::CandidateWindow::kNoSelection,
                        list.pageStart, list.count, detail);
}

void Refresh(Testbed& bed) {
    // Lay out first: the candidate window goes by where the text was drawn.
    InvalidateRect(bed.window, nullptr, FALSE);
    UpdateWindow(bed.window);
    ShowCandidates(bed);
}

void Apply(Testbed& bed, const ja::KeyCommand& command) {
    if (command.action == ja::KeyCommand::Action::Reconvert) {
        // No selection here: Henkan takes back the text just committed.
        const auto* last = bed.composer.LastCommit();
        if (!last || !bed.text.ends_with(*last)) return;
        const std::wstring text = *last;
        if (bed.composer.Reconvert(text)) bed.text.erase(bed.text.size() - text.size());
        return;
    }
    const auto outcome = ja::ApplyKey(bed.composer, command);
    if (outcome.uncommitted && bed.text.ends_with(*outcome.uncommitted)) {
        bed.text.erase(bed.text.size() - outcome.uncommitted->size());
    }
    if (outcome.committed) bed.text += *outcome.committed;
    bed.text += outcome.outside;
}

// A key the composer did not take, as a plain text box would treat it;
// whether it did anything.
bool PlainKey(Testbed& bed, WPARAM key) {
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (key != 'L') return false;
        bed.text.clear();
        bed.composer.Clear();
        bed.composer.ForgetContext();
        return true;
    }
    switch (key) {
    case VK_RETURN:
        bed.text += L'\n';
        // A new line starts a new sentence.
        bed.composer.ForgetContext();
        return true;
    case VK_BACK:
        if (!bed.text.empty()) bed.text.pop_back();
        bed.composer.ForgetContext();
        return true;
    case VK_SPACE:
        bed.text += L' ';
        return true;
    default:
        return false;
    }
}

// Whether the key was used; one that was not goes on to Windows (Alt+F4).
bool OnKey(Testbed& bed, WPARAM key) {
    if (key == VK_F12) {
        bed.composer.SetLiveConversion(!bed.composer.LiveConversion());
        Refresh(bed);
        return true;
    }
    const auto command = ja::TranslateKey(bed.composer, tekito::tsf::JapaneseKeyPressFor(key, bed.settings.japaneseIgnoreCapsLock),
                                          tekito::userdata::JapaneseKeyOptions(bed.settings));
    bool used = true;
    if (!command) {
        bed.composer.ForgetCommit();
        used = PlainKey(bed, key);
    } else {
        Apply(bed, *command);
        // A shortcut or an arrow that ended the text still does its own thing.
        if (command->letKeyThrough) used = PlainKey(bed, key);
    }
    Refresh(bed);
    return used;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_bed && wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU && OnKey(*g_bed, wParam)) {
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (g_bed) Paint(*g_bed, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
    case WM_MOVE:
        if (g_bed && g_bed->window) Refresh(*g_bed);
        break;
    case WM_ACTIVATE:
        if (g_bed && LOWORD(wParam) == WA_INACTIVE) g_bed->candidates.Hide();
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Keys come straight to this window: no input method of the system's
    // (TEKITO's own included) gets them first.
    ImmDisableIME(static_cast<DWORD>(-1));

    auto bed = std::make_unique<Testbed>();
    g_bed = bed.get();
    LoadUserData(*bed, std::wstring_view(commandLine).find(L"--defaults") != std::wstring_view::npos);
    SetUpComposer(*bed);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = &WindowProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    windowClass.lpszClassName = L"TekitoTestbed";
    RegisterClassExW(&windowClass);
    bed->window = CreateWindowExW(0, windowClass.lpszClassName, L"TEKITO Testbed", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1100, 640, nullptr, nullptr, instance, nullptr);
    if (!bed->window) return 1;
    if (!bed->candidates.Initialize(instance, [](std::size_t index) {
            if (!g_bed) return;
            ja::KeyCommand click;
            click.action = ja::KeyCommand::Action::ChooseRow;
            click.index = index;
            Apply(*g_bed, click);
            Refresh(*g_bed);
        })) {
        return 1;
    }
    ShowWindow(bed->window, show);
    UpdateWindow(bed->window);

    // No TranslateMessage: the keys are read in WM_KEYDOWN, as the text
    // service reads them.
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    g_bed = nullptr;
    return 0;
}
