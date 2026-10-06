// A window to try Japanese and English typing in without registering the
// input method: keys go through the same code the text service uses
// (JapaneseKeyPressFor, japanese::TranslateKey and ApplyKey, CandidateListFor
// in Japanese; the conversion engine and InputStateMachine in English), with
// the real candidate window, the installed Data Packs and your settings, user
// words and learning, which it reads but never writes back.
//
//   tekito_testbed.exe [--defaults] [--english] [--smart-punctuation]
//                      [--double-space-period] [--period-on-enter]
//                      [--no-next-words] [--no-doubled-words]
//
// --defaults types with the default settings and no user words or learning.
// --english starts in English (Auto) instead of Japanese. The other options
// turn an English setting on or off for this window only, over your settings.
// TEKITO_DATA_PACK_DIR picks other Data Packs (the repository's data folder,
// say); otherwise the installed ones are used. Ctrl+L clears the page; F11
// switches between Japanese and English; F12 turns live conversion on or off.
//
// What it cannot show is how applications treat a composition: caret
// placement, Word or a browser. Check those with the input method registered.
#include "Core/ConversionEngine.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/InputStateMachine.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/JapaneseKeys.h"
#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/PostalCodes.h"
#include "Core/SpecialConversions.h"
#include "Core/UserDictionary.h"
#include "Core/UserLearning.h"
#include "Tsf/CandidateWindow.h"
#include "Tsf/EditSession.h"
#include "Tsf/EnglishText.h"
#include "Tsf/JapaneseKeyPress.h"
#include "UserData/JapaneseSettings.h"
#include "UserData/SqliteUserDictionaryRepository.h"
#include "UserData/UiLanguage.h"
#include "UserData/UserSettings.h"

#include <windows.h>
#include <imm.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ja = tekito::japanese;
using tekito::tsf::KeyInput;

constexpr int kMargin = 24;

// English typing: what the text service keeps for it (TextService.h).
struct EnglishTyping {
    std::unique_ptr<tekito::IConversionEngine> engine;
    tekito::InputStateMachine state;
    // The word as typed.
    std::wstring rawText;
    // The composition as shown after the committed text.
    std::wstring composition;
    bool composing{false};
    bool sentenceStart{false};
    tekito::CapitalizationOrigin capitalization{tekito::CapitalizationOrigin::Unknown};
    // A symbol's spelling or a sum ("->", "1+2=") is being offered.
    bool symbolComposition{false};
    // Whether the candidate list is up; it is placed once the page is drawn.
    bool listShown{false};
};

struct Testbed {
    tekito::userdata::UserSettings settings;
    bool userData{false};
    ja::JapaneseLearningStore learning;
    ja::JapaneseUserDictionary userWords;
    std::shared_ptr<const ja::PostalCodes> postalCodes;
    ja::JapaneseComposer composer;
    // English: your words and what was learned, read only.
    tekito::UserDictionary englishWords;
    tekito::UserLearningStore englishLearning;
    tekito::UserLearningProvider englishLearningProvider{englishLearning};
    tekito::SocialLearningStore socialLearning;
    tekito::SocialLearningProvider socialLearningProvider{socialLearning};
    EnglishTyping en;
    bool english{false};
    tekito::tsf::CandidateWindow candidates;
    // What was typed and committed so far; the composition follows it.
    std::wstring text;
    HWND window{nullptr};
    HFONT font{nullptr};
    HFONT smallFont{nullptr};
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
    std::wstring text;  // one character as drawn (an emoji may take several units)
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
    if (repository.Load(bed.englishWords)) {
        (void)repository.LoadLearning(bed.englishLearning);
        (void)repository.LoadSocialLearning(bed.socialLearning);
    }
}

// English settings turned on or off from the command line, for this window
// only: the ones that start off can be tried without changing your settings.
void ApplyOverrides(Testbed& bed, std::wstring_view commandLine) {
    const auto has = [&](std::wstring_view flag) { return commandLine.find(flag) != std::wstring_view::npos; };
    auto& settings = bed.settings;
    if (has(L"--smart-punctuation")) settings.smartPunctuation = true;
    if (has(L"--double-space-period")) settings.doubleSpacePeriod = true;
    if (has(L"--period-on-enter")) settings.periodOnEnter = true;
    if (has(L"--no-next-words")) settings.nextWordPrediction = false;
    if (has(L"--no-doubled-words")) settings.doubledWordCheck = false;
}

// The English half, as the text service sets it up (ApplySettings).
void SetUpEnglish(Testbed& bed) {
    bed.englishLearningProvider.SetEnabled(bed.settings.learningEnabled);
    bed.socialLearningProvider.SetProfile(bed.settings.socialExpressionRange, bed.settings.socialPersonalization);
    const auto rows = static_cast<std::size_t>(bed.settings.candidateRows);
    bed.en.state.SetPageSizes(rows ? rows : 5, rows ? rows : 10);
    bed.en.state.SetDoubleSpacePeriod(bed.settings.doubleSpacePeriod);
}

// Loads the English Data Packs the first time English is used; this takes a
// few seconds.
bool EnsureEngine(Testbed& bed) {
    if (bed.en.engine) return true;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    tekito::PreloadDefaultEngineData();
    bed.en.engine =
        tekito::CreateDefaultConversionEngine(bed.englishWords, bed.englishLearningProvider, bed.socialLearningProvider);
    SetCursor(old);
    return bed.en.engine != nullptr;
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
    if (bed.smallFont) DeleteObject(bed.smallFont);
    bed.fontDpi = dpi;
    bed.font = CreateFontW(-MulDiv(22, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Yu Gothic UI");
    bed.smallFont = CreateFontW(-MulDiv(13, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Segoe UI");
}

int Scale(const Testbed& bed, int value) { return MulDiv(value, static_cast<int>(bed.fontDpi), 96); }

std::wstring StatusLine(const Testbed& bed) {
    std::wstring line = bed.english ? L"TEKITO Testbed  |  English  |  " : L"TEKITO Testbed  |  Japanese  |  ";
    line += bed.userData ? L"your settings, words and learning (read only)" : L"default settings";
    if (bed.english) {
        const auto& settings = bed.settings;
        line += settings.smartPunctuation ? L"  |  curly quotes on" : L"  |  curly quotes off";
        line += settings.doubleSpacePeriod ? L"  |  Space twice: period" : L"  |  Space twice: next";
        line += settings.nextWordPrediction ? L"  |  next words on" : L"  |  next words off";
        line += settings.doubledWordCheck ? L"  |  repeated words on" : L"  |  repeated words off";
        if (settings.periodOnEnter) line += L"  |  period on Enter";
        if (!bed.en.engine) line += L"  |  English data not loaded";
        line += L"  |  Japanese (F11)  |  Ctrl+L clears";
        return line;
    }
    line += L"  |  data: " + tekito::ExternalLexiconProvider::DataPackRoot().wstring();
    const auto missing = ja::MissingJapaneseData();
    if (!missing.empty()) {
        line += L"  |  missing:";
        for (const auto part : missing) line += L" " + std::wstring(part);
    }
    line += bed.composer.LiveConversion() ? L"  |  live conversion on (F12)" : L"  |  live conversion off (F12)";
    line += L"  |  English (F11)  |  Ctrl+L clears";
    return line;
}

// Lays out the committed text and the composition, wrapping at `width`.
std::vector<Cell> Layout(const Testbed& bed, HDC dc, int top, int width, std::size_t& caretCell) {
    std::vector<std::pair<wchar_t, Style>> chars;
    for (const wchar_t ch : bed.text) chars.push_back({ch, Style::Committed});
    const std::size_t compositionStart = chars.size();
    if (bed.english) {
        for (const wchar_t ch : bed.en.composition) chars.push_back({ch, Style::Input});
    } else {
        for (const auto& segment : bed.composer.Segments()) {
            const Style style = !segment.converted ? Style::Input : segment.focused ? Style::Focused : Style::Converted;
            for (const wchar_t ch : segment.text) chars.push_back({ch, style});
        }
    }
    const std::size_t caretUnit = !bed.english && bed.composer.IsComposing()
                                      ? compositionStart + bed.composer.CaretOffset()
                                      : chars.size();
    // Units drawn together: surrogate pairs, variation selectors, joined
    // emoji and skin tones.
    const auto joins = [&](std::size_t i) {
        const wchar_t c = chars[i].first;
        const wchar_t before = chars[i - 1].first;
        if (c >= 0xDC00 && c <= 0xDFFF) return true;
        if (c == 0xFE0E || c == 0xFE0F || c == 0x200D || c == 0x20E3) return true;
        if (before == 0x200D) return true;
        return c == 0xD83C && i + 1 < chars.size() && chars[i + 1].first >= 0xDFFB && chars[i + 1].first <= 0xDFFF;
    };
    std::vector<std::pair<std::wstring, Style>> clusters;
    caretCell = 0;
    for (std::size_t i = 0; i < chars.size(); ++i) {
        if (i > 0 && !clusters.empty() && joins(i)) {
            clusters.back().first += chars[i].first;
            continue;
        }
        if (i < caretUnit) caretCell = clusters.size() + 1;
        clusters.push_back({std::wstring(1, chars[i].first), chars[i].second});
    }

    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight + metrics.tmExternalLeading + Scale(bed, 10);
    std::vector<Cell> cells;
    int x = kMargin;
    int y = top;
    for (const auto& [text, style] : clusters) {
        if (text == L"\n") {
            cells.push_back({text, style, {x, y, x, y + metrics.tmHeight}});
            x = kMargin;
            y += lineHeight;
            continue;
        }
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        if (x + size.cx > width - kMargin && x > kMargin) {
            x = kMargin;
            y += lineHeight;
        }
        cells.push_back({text, style, {x, y, x + size.cx, y + metrics.tmHeight}});
        x += size.cx;
    }
    // Where the caret goes after the last character.
    cells.push_back({{}, Style::Committed, {x, y, x, y + metrics.tmHeight}});
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
    HGDIOBJ oldFont = SelectObject(dc, bed.smallFont);
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
        if (cell.text == L"\n") continue;
        TextOutW(dc, cell.rect.left, cell.rect.top, cell.text.c_str(), static_cast<int>(cell.text.size()));
        if (cell.style == Style::Committed) continue;
        HPEN pen = cell.style == Style::Focused ? thick : cell.style == Style::Converted ? thin : dotted;
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int y = cell.rect.bottom + Scale(bed, 1);
        // A gap between phrases, as editors draw them.
        MoveToEx(dc, cell.rect.left + 1, y, nullptr);
        LineTo(dc, cell.rect.right - 1, y);
        SelectObject(dc, oldPen);
        // In English the list goes by the whole composition, as the text
        // service places it.
        if (cell.style == Style::Focused || (bed.english && cell.style == Style::Input)) {
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

// The English candidate list, as the text service shows it (ShowCandidates,
// EnglishRows and SelectedEnglishMeaning).
void ShowEnglishCandidates(Testbed& bed) {
    const auto& state = bed.en.state;
    if (!bed.en.listShown || !bed.settings.candidateWindowEnabled) {
        bed.candidates.Hide();
        return;
    }
    auto rows = state.Candidates();
    if (bed.settings.meaningsEnabled) {
        const auto& meanings = ja::ProcessMeanings();
        for (std::size_t i = state.PageStart(); i < rows.size() && i < state.PageStart() + state.VisibleCount(); ++i) {
            rows[i].hasMeaning = meanings.Lookup(rows[i].text, {}).has_value();
        }
    }
    const std::size_t selected = state.SelectedIndex();
    const auto detail = selected < rows.size() ? MeaningFor(bed, rows[selected].text, {}) : tekito::tsf::CandidateDetail{};
    bed.candidates.SetStyle(bed.settings.candidateWindowStyle);
    bed.candidates.SetJapanese(tekito::userdata::UseJapaneseUi(bed.settings.uiLanguage));
    bed.candidates.Show(bed.hasFocusRect ? bed.focusRect : bed.caretRect, rows, selected, state.PageStart(),
                        state.VisibleCount(), detail);
}

void Refresh(Testbed& bed) {
    // Lay out first: the candidate window goes by where the text was drawn.
    InvalidateRect(bed.window, nullptr, FALSE);
    UpdateWindow(bed.window);
    if (bed.english) {
        ShowEnglishCandidates(bed);
    } else {
        ShowCandidates(bed);
    }
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

// English typing. The functions below follow TextService: TranslateEnglishKey,
// TranslateEnglishCharacter, HandleKeyInEditSessionCore and the document
// edits they make, with `text` as the document and the composition after it.
// Nothing is learned here.

bool HasBlockedModifier() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0 ||
           (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;
}

bool IsNavigationKey(WPARAM key) {
    switch (key) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_DELETE: case VK_INSERT:
        return true;
    default:
        return false;
    }
}

// The character a key types with the keyboard as it is now, if it types one.
wchar_t CharacterFor(WPARAM key) {
    BYTE keyboard[256]{};
    if (!GetKeyboardState(keyboard)) return 0;
    wchar_t buffer[8]{};
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    const int count = ToUnicodeEx(static_cast<UINT>(key), scanCode, keyboard, buffer,
                                  static_cast<int>(std::size(buffer)), 0, GetKeyboardLayout(0));
    return count == 1 && buffer[0] >= 0x20 && buffer[0] != 0x7f ? buffer[0] : 0;
}

tekito::SpecialConversionOptions SpecialOptions(const Testbed& bed) {
    return tekito::userdata::SpecialOptionsFor(bed.settings);
}

// What comes before the caret (ReadSelectionContext), composition included.
std::wstring Preceding(const Testbed& bed) {
    const std::wstring all = bed.text + bed.en.composition;
    return all.size() > 256 ? all.substr(all.size() - 256) : all;
}

// What comes before the composition (ReadCompositionContext).
std::wstring BeforeComposition(const Testbed& bed) {
    return bed.text.size() > 128 ? bed.text.substr(bed.text.size() - 128) : bed.text;
}

void ReplaceComposition(Testbed& bed, const std::wstring& written) {
    if (!bed.en.composing) return;
    std::wstring text = written;
    if (bed.settings.smartPunctuation) std::replace(text.begin(), text.end(), L'\'', L'\u2019');
    bed.en.composition = std::move(text);
}

void EndComposition(Testbed& bed) {
    bed.en.symbolComposition = false;
    bed.text += bed.en.composition;
    bed.en.composition.clear();
    bed.en.composing = false;
}

// Ends the word as it stands and forgets it.
void FinishWord(Testbed& bed) {
    EndComposition(bed);
    bed.en.listShown = false;
    bed.en.state.Reset();
    bed.en.rawText.clear();
}

bool TranslateEnglishCharacter(const Testbed& bed, wchar_t character, KeyInput& input) {
    const auto& state = bed.en.state;
    if (bed.settings.smartPunctuation && (character == L'"' || character == L'\'')) {
        const bool inWord = state.IsActive() && state.State() == tekito::CompositionState::Composing;
        if (inWord && character == L'"') {
            input.type = KeyInput::Type::Punctuation;
            input.character = L'\u201D';
            input.punctuationRole = tekito::PunctuationRole::ClauseSeparator;
            return true;
        }
        if (!inWord) {
            input.type = KeyInput::Type::SmartPunctuation;
            input.character = character;
            return true;
        }
    }
    tekito::PunctuationRole role{};
    if (tekito::tsf::TryClassifyPunctuation(character, role)) {
        if (!state.IsActive()) return false;
        input.type = KeyInput::Type::Punctuation;
        input.character = character;
        input.punctuationRole = role;
        return true;
    }
    if (!tekito::tsf::IsWordCharacter(character)) {
        if (!state.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }
    if (!state.IsActive() && !std::iswalpha(character)) return false;
    input.type = KeyInput::Type::Printable;
    input.character = character;
    return true;
}

bool TranslateEnglishKey(const Testbed& bed, WPARAM key, wchar_t character, KeyInput& input) {
    if (!bed.en.engine) return false;
    const auto& state = bed.en.state;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (HasBlockedModifier()) {
        if (!state.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }
    switch (key) {
    case VK_SPACE:
        if (!state.IsActive()) return false;
        input.type = shift ? KeyInput::Type::ShiftSpace : KeyInput::Type::Space;
        return true;
    case VK_TAB:
        if (!state.IsActive()) return false;
        input.type = shift ? KeyInput::Type::ShiftTab : KeyInput::Type::Tab;
        return true;
    case VK_BACK:
        if (!state.IsActive()) return false;
        input.type = KeyInput::Type::Backspace;
        return true;
    case VK_ESCAPE:
        if (!state.IsActive()) return false;
        input.type = KeyInput::Type::Cancel;
        return true;
    case VK_RETURN:
        if (!state.IsActive() && !bed.settings.periodOnEnter) return false;
        input.type = KeyInput::Type::Enter;
        return true;
    default:
        break;
    }
    if ((state.IsCandidateNavigationActive() || state.State() == tekito::CompositionState::Cycling) &&
        (key == VK_UP || key == VK_DOWN)) {
        input.type = key == VK_UP ? KeyInput::Type::UpArrow : KeyInput::Type::DownArrow;
        return true;
    }
    if (IsNavigationKey(key)) {
        if (!state.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }
    if (!character) return false;
    if (bed.settings.smartPunctuation && character == L'-' &&
        !(state.IsActive() && state.State() == tekito::CompositionState::Composing)) {
        input.type = KeyInput::Type::SmartPunctuation;
        input.character = character;
        return true;
    }
    if (tekito::SpecialConversions::Installed().MayEndEnglish(character, SpecialOptions(bed))) {
        input.type = KeyInput::Type::SpecialEnd;
        input.character = character;
        return true;
    }
    return TranslateEnglishCharacter(bed, character, input);
}

std::optional<tekito::SpecialConversions::Ending> SpecialEnding(const Testbed& bed, std::wstring_view preceding,
                                                                wchar_t character) {
    std::wstring text(preceding);
    text.push_back(character);
    auto ending = tekito::SpecialConversions::Installed().EnglishEnding(text, SpecialOptions(bed));
    if (ending && ending->length < 2) ending.reset();
    return ending;
}

bool ResolveSpecialKey(const Testbed& bed, KeyInput& input) {
    if (input.type != KeyInput::Type::SpecialEnd) return true;
    if (SpecialEnding(bed, Preceding(bed), input.character)) return true;
    const wchar_t character = input.character;
    input = KeyInput{};
    return TranslateEnglishCharacter(bed, character, input);
}

void RefreshEnglishCandidates(Testbed& bed) {
    auto& en = bed.en;
    const auto& settings = bed.settings;
    tekito::ConversionRequest request;
    request.rawText = en.rawText;
    request.context.precedingText = BeforeComposition(bed);
    request.context.sentenceStart = en.sentenceStart;
    request.capitalization.origin = en.capitalization;
    request.options.correctionEnabled = settings.correctionEnabled;
    request.options.commonMisspellingsEnabled = settings.commonMisspellingsEnabled;
    request.options.contextSuggestionsEnabled = settings.contextSuggestionsEnabled;
    request.options.completionEnabled = settings.completionEnabled;
    request.options.japanesePhoneticSuggestionsEnabled = settings.japanesePhoneticSuggestionsEnabled;
    request.options.socialExpressionRange = settings.socialExpressionRange;
    request.options.nextWordPrediction = settings.nextWordPrediction;
    request.options.doubledWords = settings.doubledWordCheck;
    request.options.special = SpecialOptions(bed);
    auto result = en.engine->Convert(request);
    en.state.BeginOrUpdate(en.rawText, std::move(result.candidates));
    en.listShown = true;
}

// A symbol's spelling or a sum just finished before the caret (HandleSpecialEnd).
void HandleSpecialEnd(Testbed& bed, wchar_t character) {
    auto& en = bed.en;
    const std::wstring preceding = Preceding(bed);
    const auto ending = SpecialEnding(bed, preceding, character);
    FinishWord(bed);
    const std::size_t before = ending ? ending->length - 1 : 0;
    if (!ending || bed.text.size() < before) {
        bed.text.push_back(character);
        return;
    }
    // The composition takes in what was typed before the key.
    bed.text.erase(bed.text.size() - before);
    en.composing = true;
    en.rawText = preceding.substr(preceding.size() - before);
    en.rawText.push_back(character);
    ReplaceComposition(bed, en.rawText);
    std::vector<tekito::Candidate> candidates;
    tekito::Candidate typed;
    typed.text = en.rawText;
    typed.label = tekito::SemanticLabel::Original;
    typed.isOriginal = true;
    typed.isProtected = true;
    typed.policyFlags = tekito::CandidatePolicyProtect;
    candidates.push_back(std::move(typed));
    for (const auto& offer : ending->offers) {
        if (offer == en.rawText) continue;
        tekito::Candidate candidate;
        candidate.text = offer;
        candidate.isProtected = true;
        candidate.policyFlags = tekito::CandidatePolicySuggestOnly;
        candidates.push_back(std::move(candidate));
    }
    en.symbolComposition = true;
    en.state.BeginOrUpdate(en.rawText, std::move(candidates));
    en.listShown = true;
}

// Curly quotes and dashes typed outside a word (HandleSmartPunctuation).
void HandleSmartPunctuation(Testbed& bed, wchar_t character) {
    FinishWord(bed);
    const auto preceding = Preceding(bed);
    if (character == L'-') {
        if (tekito::tsf::MakesEmDash(preceding)) {
            bed.text.back() = L'\u2014';
            return;
        }
        if (SpecialEnding(bed, preceding, character)) {
            HandleSpecialEnd(bed, character);
            return;
        }
        bed.text.push_back(L'-');
        return;
    }
    bed.text.push_back(tekito::tsf::SmartQuote(character, preceding));
}

// Whether the key was taken (S_OK in the text service); one that was not goes
// on to the page as a plain key.
bool HandleEnglishKey(Testbed& bed, const KeyInput& input) {
    using Type = KeyInput::Type;
    using tekito::ActionKind;
    auto& en = bed.en;
    auto& state = en.state;
    if (input.type == Type::SpecialEnd) {
        HandleSpecialEnd(bed, input.character);
        return true;
    }
    if (input.type == Type::SmartPunctuation) {
        HandleSmartPunctuation(bed, input.character);
        return true;
    }
    // A spelling or a sum: typing on keeps it as typed; Backspace takes its
    // last character and keeps the rest.
    if (en.symbolComposition && state.State() == tekito::CompositionState::Composing) {
        if (input.type == Type::Backspace) {
            if (!en.rawText.empty()) en.rawText.pop_back();
            ReplaceComposition(bed, en.rawText);
            FinishWord(bed);
            return true;
        }
        if (input.type == Type::Printable) FinishWord(bed);
    }

    switch (input.type) {
    case Type::Printable: {
        if (!en.composing && !state.IsActive() && !tekito::tsf::CanStartWordAfter(Preceding(bed))) {
            // Inside a URL, address, path or identifier: typed as is.
            bed.text.push_back(input.character);
            return true;
        }
        const auto action = state.OnPrintable();
        if (action.kind == ActionKind::CommitAndStartNext) {
            EndComposition(bed);
            en.listShown = false;
            en.rawText.clear();
        }
        en.composing = true;
        auto character = input.character;
        if (en.rawText.empty()) {
            en.sentenceStart = tekito::IsSentenceStart(BeforeComposition(bed));
            en.capitalization = tekito::CapitalizationOrigin::UserTyped;
            if (en.sentenceStart && std::iswlower(character)) {
                character = static_cast<wchar_t>(std::towupper(character));
                en.capitalization = tekito::CapitalizationOrigin::EngineApplied;
            }
        } else if (en.capitalization == tekito::CapitalizationOrigin::EngineApplied && en.rawText.size() == 1 &&
                   std::iswupper(character)) {
            en.rawText.front() = static_cast<wchar_t>(std::towlower(en.rawText.front()));
            en.capitalization = tekito::CapitalizationOrigin::UserTyped;
        }
        en.rawText.push_back(character);
        ReplaceComposition(bed, en.rawText);
        RefreshEnglishCandidates(bed);
        return true;
    }

    case Type::Space: {
        const auto action = state.OnSpace();
        if (action.kind == ActionKind::CommitAndStartNext) {
            ReplaceComposition(bed, action.text);
            EndComposition(bed);
            en.listShown = false;
            en.rawText.clear();
            return true;
        }
        if (action.kind != ActionKind::ReplaceComposition) return false;
        ReplaceComposition(bed, action.text);
        en.listShown = action.showCandidates;
        return true;
    }

    case Type::ShiftSpace:
    case Type::Tab:
    case Type::ShiftTab:
    case Type::UpArrow:
    case Type::DownArrow:
    case Type::CandidateSelection: {
        tekito::InputAction action;
        if (input.type == Type::CandidateSelection) {
            action = state.OnCandidateSelected(input.candidateIndex);
        } else if (input.type == Type::ShiftSpace || input.type == Type::ShiftTab || input.type == Type::UpArrow) {
            action = state.OnPreviousCandidate();
        } else {
            action = state.OnNextCandidate();
        }
        if (action.kind != ActionKind::ReplaceComposition) return false;
        ReplaceComposition(bed, action.text);
        en.listShown = true;
        return true;
    }

    case Type::Backspace: {
        const auto action = state.OnBackspace();
        if (action.kind == ActionKind::RestoreOriginal) {
            en.rawText = action.text;
            ReplaceComposition(bed, en.rawText);
            en.listShown = true;
            return true;
        }
        if (!en.rawText.empty()) en.rawText.pop_back();
        if (en.rawText.empty()) {
            ReplaceComposition(bed, en.rawText);
            FinishWord(bed);
            return true;
        }
        ReplaceComposition(bed, en.rawText);
        RefreshEnglishCandidates(bed);
        return true;
    }

    case Type::Punctuation: {
        const auto action = state.OnPunctuation(input.character, input.punctuationRole);
        if (action.kind != ActionKind::ReplaceComposition) return false;
        ReplaceComposition(bed, action.text);
        EndComposition(bed);
        en.listShown = false;
        en.rawText.clear();
        return true;
    }

    case Type::Cancel: {
        const auto action = state.OnCancel();
        if (action.kind != ActionKind::ReplaceComposition && action.kind != ActionKind::RestoreOriginal) return false;
        ReplaceComposition(bed, action.text);
        if (action.kind == ActionKind::RestoreOriginal) {
            en.rawText = action.text;
            en.listShown = true;
            return true;
        }
        EndComposition(bed);
        en.listShown = false;
        en.rawText.clear();
        return true;
    }

    case Type::Enter: {
        if (!state.IsActive()) {
            if (!bed.settings.periodOnEnter) return false;
            const auto preceding = Preceding(bed);
            if (!tekito::NeedsTerminalPeriod(preceding)) return false;
            // The period goes in place of any spaces at the end of the line.
            while (!bed.text.empty() && (bed.text.back() == L' ' || bed.text.back() == L'\t')) bed.text.pop_back();
            bed.text.push_back(L'.');
            return true;
        }
        const auto action = state.OnEnter(bed.settings.periodOnEnter);
        if (action.kind != ActionKind::CommitBeforeNewline && action.kind != ActionKind::EndComposition) return false;
        ReplaceComposition(bed, action.text);
        EndComposition(bed);
        en.listShown = false;
        en.rawText.clear();
        return true;
    }

    case Type::EndComposition:
        FinishWord(bed);
        return true;

    default:
        return false;
    }
}

// A key English did not take, as a plain text box would treat it; whether
// it did anything.
bool EnglishPlainKey(Testbed& bed, WPARAM key, wchar_t character) {
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) return false;
    // The page types at the caret, after the word.
    if (bed.en.composing) FinishWord(bed);
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (key != 'L') return false;
        bed.text.clear();
        return true;
    }
    switch (key) {
    case VK_RETURN:
        bed.text += L'\n';
        return true;
    case VK_BACK:
        if (!bed.text.empty()) bed.text.pop_back();
        return true;
    case VK_TAB:
    case VK_ESCAPE:
        return true;
    default:
        if (!character) return false;
        bed.text += character;
        return true;
    }
}

bool OnEnglishKey(Testbed& bed, WPARAM key) {
    const wchar_t character = CharacterFor(key);
    KeyInput input{};
    bool used = true;
    if (!TranslateEnglishKey(bed, key, character, input) || !ResolveSpecialKey(bed, input)) {
        used = EnglishPlainKey(bed, key, character);
    } else {
        // Keys that end the word and still reach the page (LetsKeyThrough).
        const bool through = input.type == KeyInput::Type::EndComposition ||
                             (input.type == KeyInput::Type::Enter && !bed.en.state.IsCandidateNavigationActive());
        if (!HandleEnglishKey(bed, input) || through) used = EnglishPlainKey(bed, key, character);
    }
    Refresh(bed);
    return used;
}

// F11: English and Japanese in turn. What is being typed is committed first.
void SwitchMode(Testbed& bed) {
    if (bed.english) {
        FinishWord(bed);
    } else {
        bed.text += bed.composer.Commit();
        bed.composer.ForgetCommit();
        bed.composer.ForgetContext();
    }
    bed.candidates.Hide();
    bed.english = !bed.english;
    if (bed.english) EnsureEngine(bed);
    Refresh(bed);
}

// Whether the key was used; one that was not goes on to Windows (Alt+F4).
bool OnKey(Testbed& bed, WPARAM key) {
    if (key == VK_F11) {
        SwitchMode(bed);
        return true;
    }
    if (bed.english) return OnEnglishKey(bed, key);
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
    const std::wstring_view options(commandLine);
    LoadUserData(*bed, options.find(L"--defaults") != std::wstring_view::npos);
    ApplyOverrides(*bed, options);
    SetUpComposer(*bed);
    SetUpEnglish(*bed);
    if (options.find(L"--english") != std::wstring_view::npos) bed->english = EnsureEngine(*bed);

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
            if (g_bed->english) {
                KeyInput click{};
                click.type = KeyInput::Type::CandidateSelection;
                click.candidateIndex = index;
                if (g_bed->en.state.IsActive()) (void)HandleEnglishKey(*g_bed, click);
                Refresh(*g_bed);
                return;
            }
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
