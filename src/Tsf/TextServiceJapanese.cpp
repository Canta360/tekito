// The ja-JP profile: switching among Japanese, English Auto and Direct, and
// typing Japanese. The English typing path is in TextService.cpp.

#include "Tsf/TextService.h"

#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/PostalCodes.h"
#include "Tsf/JapaneseKeyPress.h"
#include "Tsf/Compartments.h"
#include "Tsf/DisplayAttributes.h"
#include "Tsf/Diagnostics.h"
#include "Tsf/TekitoGuids.h"
#include "UserData/UiLanguage.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

namespace tekito::tsf {
namespace {

// Virtual keys of a Japanese keyboard. Hankaku/Zenkaku sends 0xF3 and 0xF4
// in turn (and VK_KANJI with Alt); Hiragana/Katakana sends 0xF2, or 0xF1
// with Shift. IME On/Off are the keys some keyboards (and remappers) send.
constexpr WPARAM kVkHankakuZenkakuSbcs = 0xF3;
constexpr WPARAM kVkHankakuZenkakuDbcs = 0xF4;
constexpr WPARAM kVkHiragana = 0xF2;
constexpr WPARAM kVkKatakana = 0xF1;
constexpr WPARAM kVkImeOn = 0x16;
constexpr WPARAM kVkImeOff = 0x1A;

bool KeyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }

bool HasCommandModifier() {
    return KeyDown(VK_CONTROL) || KeyDown(VK_MENU) || KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
}

// The process's Japanese data for `composer`; what is missing is noted once.
void AttachJapanese(japanese::JapaneseComposer& composer) {
    composer.SetTable(japanese::ProcessRomajiTable());
    japanese::AttachJapaneseData(composer);
    static std::once_flag noted;
    std::call_once(noted, [] {
        for (const auto missing : japanese::MissingJapaneseData()) {
            const std::wstring line = L"Japanese " + std::wstring(missing) + L" unavailable";
            Trace(line.c_str());
        }
    });
}

}  // namespace

InputMode TextService::SharedMode() const noexcept {
    if (!runtimeMode_) return mode_;
    if (!japaneseProfile_) return runtimeMode_->Mode();
    const auto mode = runtimeMode_->JapaneseMode();
    return mode == InputMode::Japanese && !JapaneseModeAvailable() ? InputMode::Convert : mode;
}

InputMode TextService::ToggledMode() const noexcept {
    if (!JapaneseModeAvailable()) {
        return mode_ == InputMode::Convert ? InputMode::Direct : InputMode::Convert;
    }
    switch (userSettings_.japaneseSwitchOrder) {
    case 2:
        // Japanese, Auto, Direct, and round again.
        return mode_ == InputMode::Japanese ? InputMode::Convert
               : mode_ == InputMode::Convert ? InputMode::Direct
                                             : InputMode::Japanese;
    case 1:
        return mode_ == InputMode::Japanese ? InputMode::Direct : InputMode::Japanese;
    default:
        return mode_ == InputMode::Japanese ? InputMode::Convert : InputMode::Japanese;
    }
}

void TextService::UpdateActiveProfile() {
    bool japanese = false;
    ComPtr<ITfInputProcessorProfileMgr> profiles;
    TF_INPUTPROCESSORPROFILE active{};
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(profiles.Put()))) &&
        SUCCEEDED(profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active))) {
        japanese = IsEqualCLSID(active.clsid, CLSID_TekitoTextService) &&
                   IsEqualGUID(active.guidProfile, GUID_TekitoJapaneseProfile);
    } else {
        japanese = PRIMARYLANGID(LOWORD(reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)))) ==
                   LANG_JAPANESE;
    }
    japaneseProfile_ = japanese;
    if (japanese) {
        AttachJapanese(japanese_);
        RefreshJapaneseUserWords();
    }
}

void TextService::RefreshJapaneseUserWords() {
    const auto* parts = japanese::ProcessUserParts();
    if (!parts) {
        japanese_.SetUserDictionary(nullptr);
        return;
    }
    japaneseUserDictionary_.Set(japaneseUserWords_, *parts);
    postalCodes_ = japanese::ProcessPostalCodes();
    japanese_.SetPostalCodes(postalCodes_.get());
    japanese_.SetUserDictionary(japaneseUserDictionary_.Empty() ? nullptr : &japaneseUserDictionary_);
}

void TextService::SetJapaneseProfile(bool japanese) {
    if (japanese == japaneseProfile_) return;
    Trace(japanese ? L"Profile Japanese" : L"Profile English");
    if (japanese_.IsComposing() || state_.IsActive()) FinishCompositionLater();
    japaneseProfile_ = japanese;
    activationSettling_ = japanese;
    if (japanese) {
        AttachJapanese(japanese_);
        RefreshJapaneseUserWords();
    }
    SetInputMode(SharedMode());
}

HRESULT TextService::OnActivated(DWORD profileType, LANGID, REFCLSID clsid, REFGUID, REFGUID profile,
                                 HKL, DWORD flags) {
    if (profileType != TF_PROFILETYPE_INPUTPROCESSOR || !IsEqualCLSID(clsid, CLSID_TekitoTextService) ||
        (flags & TF_IPSINK_FLAG_ACTIVE) == 0) {
        return S_OK;
    }
    SetJapaneseProfile(IsEqualGUID(profile, GUID_TekitoJapaneseProfile));
    return S_OK;
}

void TextService::SyncModeCompartments() {
    if (!threadManager_ || clientId_ == TF_CLIENTID_NULL) return;
    updatingCompartments_ = true;
    WriteCompartment(ThreadCompartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE).Get(),
                     clientId_, mode_ == InputMode::Direct ? 0 : 1);
    if (japaneseProfile_ && mode_ != InputMode::Direct) {
        LONG conversion = TF_CONVERSIONMODE_ALPHANUMERIC;
        if (mode_ == InputMode::Japanese) {
            conversion = TF_CONVERSIONMODE_NATIVE | TF_CONVERSIONMODE_FULLSHAPE | TF_CONVERSIONMODE_ROMAN;
            if (japanese_.InputForm() == japanese::KanaForm::Katakana) {
                conversion |= TF_CONVERSIONMODE_KATAKANA;
            }
        }
        WriteCompartment(
            ThreadCompartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION).Get(),
            clientId_, conversion);
    }
    updatingCompartments_ = false;
}

// Applications (and Windows) turn the input method on and off through the
// compartments: follow them.
HRESULT TextService::OnChange(REFGUID compartment) {
    if (updatingCompartments_) return S_OK;
    const bool openCloseChanged = IsEqualGUID(compartment, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    const bool conversionChanged =
        IsEqualGUID(compartment, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    if (!openCloseChanged && !conversionChanged) return S_OK;

    LONG open = 0;
    if (!ReadCompartment(ThreadCompartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE).Get(),
                         open)) {
        return S_OK;
    }
    if (!JapaneseModeAvailable()) {
        if (!openCloseChanged) return S_OK;
        const auto mode = open != 0 ? InputMode::Convert : InputMode::Direct;
        if (mode != mode_) ChangeInputMode(mode);
        return S_OK;
    }
    if (activationSettling_) {
        // Windows' own reset after activation: keep TEKITO's mode.
        SyncModeCompartments();
        return S_OK;
    }

    InputMode target = mode_;
    if (open == 0) {
        target = InputMode::Direct;
    } else if (openCloseChanged) {
        if (mode_ == InputMode::Direct) target = InputMode::Japanese;
    } else {
        LONG conversion = 0;
        if (!ReadCompartment(
                ThreadCompartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION).Get(),
                conversion)) {
            return S_OK;
        }
        if ((conversion & TF_CONVERSIONMODE_NATIVE) != 0) {
            target = InputMode::Japanese;
            japanese_.SetInputForm((conversion & TF_CONVERSIONMODE_KATAKANA) != 0
                                       ? japanese::KanaForm::Katakana
                                       : japanese::KanaForm::Hiragana);
        } else {
            target = InputMode::Convert;
        }
    }
    if (target != mode_) ChangeInputMode(target);
    return S_OK;
}

bool TextService::TranslateModeKey(WPARAM wParam, ModeKey& key) const {
    if (ProcessPassesThrough()) return false;
    switch (wParam) {
    case kVkHankakuZenkakuSbcs:
    case kVkHankakuZenkakuDbcs:
    case VK_KANJI:
        // Hankaku/Zenkaku switches only when it is the switch key; otherwise
        // it is taken and does nothing, so no other key is heard as one.
        key.mode = ToggledMode();
        key.ignored = userSettings_.toggleKey != 1;
        return true;
    default:
        break;
    }
    if (!JapaneseModeAvailable()) {
        if (wParam == kVkImeOn || wParam == kVkImeOff) {
            key.mode = wParam == kVkImeOn ? InputMode::Convert : InputMode::Direct;
            return true;
        }
        return false;
    }

    const auto english = userSettings_.japaneseProfileEnglishMode == InputMode::Direct
                             ? InputMode::Direct
                             : InputMode::Convert;
    switch (wParam) {
    case VK_CONVERT:
        // In a composition Henkan converts; in Japanese it has nothing to do.
        if (japanese_.IsComposing() || mode_ == InputMode::Japanese) return false;
        key.mode = InputMode::Japanese;
        return true;
    case VK_NONCONVERT:
        if (japanese_.IsComposing()) return false;
        // To English; pressed again in English, between Auto and Direct.
        key.mode = mode_ == InputMode::Japanese ? english
                   : mode_ == InputMode::Convert ? InputMode::Direct
                                                 : InputMode::Convert;
        return true;
    case kVkHiragana:
    case kVkKatakana:
        key.mode = InputMode::Japanese;
        key.setsInputForm = true;
        key.inputForm = wParam == kVkKatakana || KeyDown(VK_SHIFT) ? japanese::KanaForm::Katakana
                                                                   : japanese::KanaForm::Hiragana;
        return true;
    case kVkImeOn:
        key.mode = InputMode::Japanese;
        return true;
    case kVkImeOff:
        key.mode = english;
        return true;
    default:
        return false;
    }
}

void TextService::ApplyModeKey(ITfContext* context, const ModeKey& key) {
    if (key.ignored) return;
    // What is being typed is committed as it stands before the switch.
    if (context && (japanese_.IsComposing() || state_.IsActive())) {
        KeyInput commit{};
        commit.type = japanese_.IsComposing() ? KeyInput::Type::JapaneseCommit
                                              : KeyInput::Type::EndComposition;
        [[maybe_unused]] const auto ignored = RequestKeyEditSession(context, commit);
    }
    if (key.setsInputForm) japanese_.SetInputForm(key.inputForm);
    if (key.mode != mode_) {
        ChangeInputMode(key.mode);
    } else {
        activationSettling_ = false;
        SyncModeCompartments();
        if (modeLangBarItem_) modeLangBarItem_->NotifyUpdate();
    }
    ShowModeIndicator(context);
}

// The character a key types with the current layout and modifiers, or 0.
wchar_t TypedCharacter(WPARAM wParam) {
    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState)) return 0;
    wchar_t buffer[8]{};
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(wParam), MAPVK_VK_TO_VSC);
    const int count = ToUnicodeEx(static_cast<UINT>(wParam), scanCode, keyboardState, buffer,
                                  static_cast<int>(std::size(buffer)), 0, GetKeyboardLayout(0));
    return count == 1 ? buffer[0] : 0;
}

bool TextService::TranslateEnglishSegmentKey(WPARAM wParam, KeyInput& input) {
    if (HasCommandModifier()) {
        if (!englishSegment_ || !TranslateEnglishKey(wParam, input)) return false;
        input.englishSegment = true;
        return true;
    }
    if (!englishSegment_) {
        // Shift+letter with nothing typed starts an English word.
        if (japanese_.IsComposing() || !KeyDown(VK_SHIFT)) return false;
        const wchar_t ch = TypedCharacter(wParam);
        if (!(ch >= L'A' && ch <= L'Z')) return false;
        if (!TranslateEnglishKey(wParam, input)) return false;
        input.englishSegment = true;
        return true;
    }
    if (wParam == VK_RETURN) {
        // Enter settles the word and goes back to Japanese; no new line.
        input.type = KeyInput::Type::EnglishSegmentCommit;
        input.englishSegment = true;
        return true;
    }
    // After the word and its space, a letter without Shift is Japanese again.
    const bool afterWord = !state_.IsActive() || state_.State() == CompositionState::Boundary ||
                           (state_.State() == CompositionState::Cycling && !state_.IsCandidateNavigationActive());
    const wchar_t ch = TypedCharacter(wParam);
    if (afterWord && !KeyDown(VK_SHIFT) && ch > 0x20 && ch < 0x7F) {
        input.type = KeyInput::Type::EnglishSegmentEnd;
        input.character = ch;
        input.englishSegment = true;
        return true;
    }
    if (!TranslateEnglishKey(wParam, input)) return false;
    input.englishSegment = true;
    return true;
}

void TextService::EndEnglishSegment() {
    englishSegment_ = false;
    if (mode_ == InputMode::Japanese) state_.SetInputMode(InputMode::Direct);
}

HRESULT TextService::HandleEnglishSegmentEnd(ITfContext* context, TfEditCookie editCookie,
                                             const KeyInput& input) {
    candidateWindow_.Hide();
    if (composition_) {
        // The word stays as shown, with any space the user typed after it.
        const HRESULT hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
    }
    state_.Reset();
    rawText_.clear();
    EndEnglishSegment();
    if (input.type != KeyInput::Type::EnglishSegmentEnd) return S_OK;
    // The letter starts Japanese typing.
    KeyInput typed{};
    typed.type = KeyInput::Type::Printable;
    typed.character = input.character;
    return HandleJapaneseKey(context, editCookie, typed);
}

bool TextService::TranslateJapaneseKey(WPARAM wParam, KeyInput& input) {
    japanese::KeyOptions options = userdata::JapaneseKeyOptions(userSettings_);
    options.pageSize = japanesePage_;
    const auto command = japanese::TranslateKey(japanese_, JapaneseKeyPressFor(wParam), options);
    if (!command) return false;
    // A key that only ends the text goes on to the application (LetsKeyThrough).
    input.type = command->letKeyThrough ? KeyInput::Type::EndComposition : KeyInput::Type::JapaneseKey;
    input.japaneseKey = *command;
    return true;
}

HRESULT TextService::HandleJapaneseKey(ITfContext* context, TfEditCookie editCookie,
                                       const KeyInput& input) {
    using Action = japanese::KeyCommand::Action;
    japanese::KeyCommand command;
    switch (input.type) {
    case KeyInput::Type::JapaneseKey:
        command = input.japaneseKey;
        break;
    case KeyInput::Type::CandidateSelection:
        command.action = Action::ChooseRow;
        command.index = input.candidateIndex;
        break;
    case KeyInput::Type::Enter:
    case KeyInput::Type::JapaneseCommit:
    case KeyInput::Type::EndComposition:
        command.action = Action::Commit;
        break;
    default:
        return S_FALSE;
    }
    if (command.action == Action::Commit) {
        const HRESULT hr = CommitJapanese(context, editCookie);
        // An English word left from before a switch ends as typed.
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        return hr;
    }
    if (command.action == Action::Insert) {
        // After a conversion, typing on commits it and starts anew (live
        // conversion types on).
        if (japanese_.CommitsBeforeTyping()) {
            const HRESULT hr = CommitJapanese(context, editCookie);
            if (FAILED(hr)) return hr;
        }
        if (!japanese_.IsComposing()) CheckJapaneseContext(context, editCookie);
        const HRESULT hr = EnsureComposition(context, editCookie);
        if (FAILED(hr)) return hr;
    }
    const auto outcome = japanese::ApplyKey(japanese_, command);
    if (!outcome.outside.empty()) return InsertAtSelection(context, editCookie, outcome.outside);
    if (outcome.committed) {
        // A prediction chosen with a click.
        candidateWindow_.Hide();
        return WriteJapaneseCommit(context, editCookie, *outcome.committed);
    }
    return ShowJapanesePreedit(context, editCookie);
}

HRESULT TextService::ShowJapanesePreedit(ITfContext* context, TfEditCookie editCookie) {
    if (!japanese_.IsComposing()) {
        candidateWindow_.Hide();
        if (composition_) {
            const HRESULT hr = ReplaceComposition(context, editCookie, std::wstring{});
            if (FAILED(hr)) return hr;
        }
        return EndComposition(editCookie);
    }
    HRESULT hr = EnsureComposition(context, editCookie);
    if (FAILED(hr)) return hr;
    hr = ReplaceJapaneseComposition(context, editCookie);
    if (FAILED(hr)) return hr;
    ShowJapaneseCandidates(context, editCookie);
    return S_OK;
}

HRESULT TextService::ReplaceJapaneseComposition(ITfContext* context, TfEditCookie editCookie) {
    if (!composition_) return E_UNEXPECTED;
    ComPtr<ITfRange> range;
    HRESULT hr = composition_->GetRange(range.Put());
    if (FAILED(hr)) return hr;
    const auto segments = japanese_.Segments();
    std::wstring text;
    for (const auto& segment : segments) text += segment.text;
    hr = range->SetText(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (FAILED(hr)) return hr;

    LONG offset = 0;
    for (const auto& segment : segments) {
        const auto length = static_cast<LONG>(segment.text.size());
        ComPtr<ITfRange> part;
        LONG moved = 0;
        if (length > 0 && SUCCEEDED(range->Clone(part.Put())) &&
            SUCCEEDED(part->Collapse(editCookie, TF_ANCHOR_START)) &&
            SUCCEEDED(part->ShiftEnd(editCookie, offset + length, &moved, nullptr)) &&
            SUCCEEDED(part->ShiftStart(editCookie, offset, &moved, nullptr))) {
            const auto underline = !segment.converted ? JapaneseUnderline::Input
                                   : segment.focused  ? JapaneseUnderline::Focused
                                                      : JapaneseUnderline::Converted;
            (void)ApplyDisplayAttribute(context, editCookie, part.Get(), JapaneseUnderlineGuid(underline));
        }
        offset += length;
    }

    // The caret: where typing goes before conversion, the end after.
    hr = range->Collapse(editCookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
    LONG moved = 0;
    hr = range->ShiftStart(editCookie, static_cast<LONG>(japanese_.CaretOffset()), &moved, nullptr);
    if (FAILED(hr)) return hr;
    hr = range->Collapse(editCookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
    TF_SELECTION selection{};
    selection.range = range.Get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    return context->SetSelection(editCookie, 1, &selection);
}

RECT TextService::JapaneseCandidateAnchor(ITfContext* context, TfEditCookie editCookie) const {
    // Under the phrase being converted.
    LONG offset = 0;
    LONG length = 0;
    for (const auto& segment : japanese_.Segments()) {
        if (segment.focused) {
            length = static_cast<LONG>(segment.text.size());
            break;
        }
        offset += static_cast<LONG>(segment.text.size());
    }
    ComPtr<ITfContextView> view;
    ComPtr<ITfRange> range;
    ComPtr<ITfRange> part;
    RECT rect{};
    BOOL clipped = FALSE;
    LONG moved = 0;
    if (context && composition_ && length > 0 && SUCCEEDED(context->GetActiveView(view.Put())) &&
        SUCCEEDED(composition_->GetRange(range.Put())) && SUCCEEDED(range->Clone(part.Put())) &&
        SUCCEEDED(part->Collapse(editCookie, TF_ANCHOR_START)) &&
        SUCCEEDED(part->ShiftEnd(editCookie, offset + length, &moved, nullptr)) &&
        SUCCEEDED(part->ShiftStart(editCookie, offset, &moved, nullptr)) &&
        SUCCEEDED(view->GetTextExt(editCookie, part.Get(), &rect, &clipped)) &&
        (rect.right > rect.left || rect.bottom > rect.top)) {
        return rect;
    }
    return GetCandidateAnchor(context, editCookie);
}

CandidateDetail TextService::MeaningFor(std::wstring_view text, std::wstring_view reading) const {
    if (!userSettings_.meaningsEnabled || text.empty()) return {};
    const auto meaning = japanese::ProcessMeanings().Lookup(text, reading);
    if (!meaning) return {};
    return {meaning->headword, meaning->senses};
}

void TextService::MarkMeanings(std::vector<Candidate>& candidates, std::size_t first, std::size_t count,
                               std::wstring_view reading) const {
    if (!userSettings_.meaningsEnabled) return;
    const auto& meanings = japanese::ProcessMeanings();
    for (std::size_t i = first; i < candidates.size() && i < first + count; ++i) {
        candidates[i].hasMeaning = meanings.Lookup(candidates[i].text, reading).has_value();
    }
}

std::vector<Candidate> TextService::EnglishRows() const {
    auto rows = state_.Candidates();
    MarkMeanings(rows, state_.PageStart(), state_.VisibleCount());
    return rows;
}

CandidateDetail TextService::SelectedEnglishMeaning() const {
    const auto& candidates = state_.Candidates();
    const std::size_t selected = state_.SelectedIndex();
    return selected < candidates.size() ? MeaningFor(candidates[selected].text) : CandidateDetail{};
}

void TextService::ShowJapaneseCandidates(ITfContext* context, TfEditCookie editCookie) {
    using Kind = japanese::CandidateList::Kind;
    const auto list = japanese::CandidateListFor(japanese_, japanesePage_);
    if (TraceEnabled()) {
        wchar_t line[96]{};
        swprintf_s(line, L"Japanese list converted=%d open=%d predictions=%zu", japanese_.IsConverted() ? 1 : 0,
                   japanese_.IsCandidateListOpen() ? 1 : 0, japanese_.Predictions().size());
        Trace(line);
    }
    if (list.kind == Kind::None || !userSettings_.candidateWindowEnabled) {
        candidateWindow_.Hide();
        return;
    }
    std::vector<Candidate> rows;
    rows.reserve(list.rows.size());
    for (std::size_t i = 0; i < list.rows.size(); ++i) {
        Candidate row;
        row.text = list.rows[i].text;
        row.id = list.rows[i].number;
        if (list.rows[i].suggestion) row.label = SemanticLabel::Suggestion;
        // Only the rows on the page shown are looked up.
        row.hasMeaning = userSettings_.meaningsEnabled && i >= list.pageStart && i < list.pageStart + list.count &&
                         japanese::ProcessMeanings().Lookup(list.rows[i].text, list.rows[i].reading).has_value();
        rows.push_back(std::move(row));
    }
    const auto& selected = list.selected;
    const CandidateDetail detail = selected && *selected < list.rows.size()
                                       ? MeaningFor(list.rows[*selected].text, list.rows[*selected].reading)
                                       : CandidateDetail{};
    // Predictions follow the caret; candidates go under their phrase.
    const RECT anchor = list.kind == Kind::Predictions ? GetCandidateAnchor(context, editCookie)
                                                       : JapaneseCandidateAnchor(context, editCookie);
    candidateAnchor_ = anchor;
    candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
    candidateWindow_.SetJapanese(userdata::UseJapaneseUi(userSettings_.uiLanguage));
    candidateWindow_.Show(anchor, rows, selected ? *selected : CandidateWindow::kNoSelection, list.pageStart,
                          list.count, detail);
}

HRESULT TextService::CommitJapanese(ITfContext* context, TfEditCookie editCookie) {
    candidateWindow_.Hide();
    if (!japanese_.IsComposing()) return EndComposition(editCookie);
    return WriteJapaneseCommit(context, editCookie, japanese_.Commit());
}

HRESULT TextService::WriteJapaneseCommit(ITfContext* context, TfEditCookie editCookie, const std::wstring& text) {
    lastJapaneseCommit_ = text;
    if (!composition_) {
        return text.empty() ? S_OK : InsertAtSelection(context, editCookie, text);
    }
    const HRESULT hr = ReplaceComposition(context, editCookie, text);
    if (FAILED(hr)) return hr;
    return EndComposition(editCookie);
}

}  // namespace tekito::tsf
