// The ja-JP profile: switching among Japanese, English Auto and Direct, and
// typing Japanese. The English typing path is in TextService.cpp.

#include "Tsf/TextService.h"

#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/RomajiTable.h"
#include "Tsf/Compartments.h"
#include "Tsf/DisplayAttributes.h"
#include "Tsf/Diagnostics.h"
#include "Tsf/TekitoGuids.h"
#include "UserData/UiLanguage.h"

#include <algorithm>
#include <iterator>
#include <memory>

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

// Keys that move the caret or focus: they settle the text and go on to the
// application.
bool IsCaretKey(WPARAM key) {
    switch (key) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_DELETE: case VK_INSERT: case VK_TAB:
        return true;
    default:
        return false;
    }
}

// Loaded once per process: the table is a few kilobytes.
const japanese::RomajiTable* ProcessRomajiTable() {
    static const std::unique_ptr<japanese::RomajiTable> table = [] {
        auto loaded = japanese::RomajiTable::Load(ExternalLexiconProvider::DataPackRoot() /
                                                  L"japanese-romaji");
        if (!loaded) Trace(L"Japanese romaji table unavailable");
        return loaded;
    }();
    return table.get();
}

// The japanese-core pack (and the language model and the loanwords for
// katakana), mapped once per process: mapping takes well under a
// millisecond and the pages are shared with every other process. Without
// japanese-core, Japanese still types kana, and Space switches kana;
// without the language model, conversion goes by the parts of speech alone;
// without the loanwords, katakana has no English candidates.
struct JapaneseData {
    japanese::JapaneseDictionary dictionary;
    japanese::ConnectionMatrix matrix;
    japanese::LanguageModel model;
    japanese::Loanwords loanwords;
    std::unique_ptr<japanese::JapaneseConverter> converter;
    std::unique_ptr<japanese::KeyConverter> keys;
};

const JapaneseData* ProcessJapaneseData() {
    static const std::unique_ptr<JapaneseData> data = [] {
        auto loaded = std::make_unique<JapaneseData>();
        const auto root = ExternalLexiconProvider::DataPackRoot();
        if (!loaded->dictionary.Open(root / L"japanese-core" / L"dictionary.bin") ||
            !loaded->matrix.Open(root / L"japanese-core" / L"connection.bin")) {
            Trace(L"Japanese dictionary unavailable");
            return std::unique_ptr<JapaneseData>{};
        }
        loaded->converter = std::make_unique<japanese::JapaneseConverter>(loaded->dictionary, loaded->matrix);
        if (loaded->model.Open(root / L"japanese-lm")) {
            loaded->converter->SetLanguageModel(&loaded->model);
        } else {
            Trace(L"Japanese language model unavailable");
        }
        if (const auto* table = ProcessRomajiTable()) {
            loaded->keys = std::make_unique<japanese::KeyConverter>(loaded->dictionary, loaded->matrix,
                                                                    *loaded->converter, *table);
        }
        if (!loaded->loanwords.Open(root / L"japanese-loanwords")) Trace(L"Japanese loanwords unavailable");
        return loaded;
    }();
    return data.get();
}

// The meaning packs, mapped on first use; lookups read them in place.
const japanese::MeaningDictionary& ProcessMeanings() {
    static const std::unique_ptr<japanese::MeaningDictionary> meanings = [] {
        auto opened = std::make_unique<japanese::MeaningDictionary>();
        if (!opened->Open(ExternalLexiconProvider::DataPackRoot())) Trace(L"Meaning packs unavailable");
        return opened;
    }();
    return *meanings;
}

void AttachJapaneseData(japanese::JapaneseComposer& composer) {
    const auto* data = ProcessJapaneseData();
    composer.SetConverter(data ? data->converter.get() : nullptr);
    composer.SetKeyConverter(data ? data->keys.get() : nullptr);
    composer.SetLoanwords(data && data->loanwords.IsOpen() ? &data->loanwords : nullptr);
}

}  // namespace

InputMode TextService::SharedMode() const noexcept {
    if (!runtimeMode_) return mode_;
    return japaneseProfile_ ? runtimeMode_->JapaneseMode() : runtimeMode_->Mode();
}

InputMode TextService::ToggledMode() const noexcept {
    if (!japaneseProfile_) {
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
        japanese_.SetTable(ProcessRomajiTable());
        AttachJapaneseData(japanese_);
    }
}

void TextService::SetJapaneseProfile(bool japanese) {
    if (japanese == japaneseProfile_) return;
    Trace(japanese ? L"Profile Japanese" : L"Profile English");
    if (japanese_.IsComposing() || state_.IsActive()) FinishCompositionLater();
    japaneseProfile_ = japanese;
    activationSettling_ = japanese;
    if (japanese) {
        japanese_.SetTable(ProcessRomajiTable());
        AttachJapaneseData(japanese_);
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
    if (!japaneseProfile_) {
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
    if (!japaneseProfile_) {
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
    const bool composing = japanese_.IsComposing();
    if (HasCommandModifier()) {
        // Shortcuts act on the committed text.
        if (!composing) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }
    if (japanese_.IsConverted()) {
        const bool shift = KeyDown(VK_SHIFT);
        switch (wParam) {
        case VK_SPACE:
            input.type = shift ? KeyInput::Type::JapanesePreviousCandidate : KeyInput::Type::JapaneseConvert;
            return true;
        case VK_DOWN:
            input.type = KeyInput::Type::JapaneseNextCandidate;
            return true;
        case VK_UP:
            input.type = KeyInput::Type::JapanesePreviousCandidate;
            return true;
        case VK_LEFT:
        case VK_RIGHT:
            input.type = shift ? KeyInput::Type::JapaneseResize : KeyInput::Type::JapaneseMoveFocus;
            input.delta = wParam == VK_LEFT ? -1 : 1;
            return true;
        default:
            break;
        }
        // With the list open, 1-9 choose from the page shown.
        const bool digit = (wParam >= '1' && wParam <= '9') || (wParam >= VK_NUMPAD1 && wParam <= VK_NUMPAD9);
        if (digit && japanese_.IsCandidateListOpen() && !shift) {
            const std::size_t number = wParam >= VK_NUMPAD1 ? wParam - VK_NUMPAD1 : wParam - '1';
            const std::size_t page = japanese_.FocusedSelection() / japanesePage_ * japanesePage_;
            const auto* candidates = japanese_.FocusedCandidates();
            if (candidates && page + number < candidates->size()) {
                input.type = KeyInput::Type::JapaneseSelectCandidate;
                input.candidateIndex = page + number;
                return true;
            }
        }
    }
    if (composing && !japanese_.IsConverted() && !japanese_.Predictions().empty()) {
        const bool chosen = japanese_.ChosenPrediction().has_value();
        switch (wParam) {
        case VK_TAB:
            input.type = KeyDown(VK_SHIFT) ? KeyInput::Type::JapanesePreviousPrediction
                                           : KeyInput::Type::JapaneseNextPrediction;
            return true;
        case VK_DOWN:
            input.type = KeyInput::Type::JapaneseNextPrediction;
            return true;
        case VK_UP:
            if (!chosen) break;
            input.type = KeyInput::Type::JapanesePreviousPrediction;
            return true;
        case VK_ESCAPE:
            if (!chosen) break;
            input.type = KeyInput::Type::JapaneseClearPrediction;
            return true;
        default:
            break;
        }
    }
    if (composing && !japanese_.IsConverted()) {
        // Before conversion the arrows edit the typed text, as in Microsoft IME.
        switch (wParam) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
            input.type = KeyInput::Type::JapaneseMoveCaret;
            input.delta = wParam == VK_LEFT ? -1 : wParam == VK_RIGHT ? 1 : wParam == VK_HOME ? -100000 : 100000;
            return true;
        case VK_DELETE:
            input.type = KeyInput::Type::JapaneseDelete;
            return true;
        default:
            break;
        }
    }
    switch (wParam) {
    case VK_SPACE:
    case VK_CONVERT:
        if (composing) {
            input.type = KeyInput::Type::JapaneseConvert;
            return true;
        }
        if (wParam == VK_SPACE) {
            bool fullWidth = userSettings_.japaneseSpaceWidth != 1;
            if (KeyDown(VK_SHIFT)) fullWidth = !fullWidth;
            // A half-width space is the application's own.
            if (!fullWidth) return false;
            input.type = KeyInput::Type::InsertCharacter;
            input.character = L'　';
            return true;
        }
        return false;
    case VK_NONCONVERT:
        if (!composing) return false;
        input.type = KeyInput::Type::JapaneseCycleKana;
        return true;
    case VK_RETURN:
        if (!composing) return false;
        input.type = KeyInput::Type::Enter;
        return true;
    case VK_BACK:
        if (!composing) return false;
        input.type = KeyInput::Type::Backspace;
        return true;
    case VK_ESCAPE:
        if (!composing) return false;
        input.type = KeyInput::Type::Cancel;
        return true;
    case VK_F6:
    case VK_F7:
    case VK_F8:
    case VK_F9:
    case VK_F10: {
        if (!composing) return false;
        constexpr japanese::KanaForm forms[] = {
            japanese::KanaForm::Hiragana, japanese::KanaForm::Katakana,
            japanese::KanaForm::HalfWidthKatakana, japanese::KanaForm::FullWidthAlphanumeric,
            japanese::KanaForm::HalfWidthAlphanumeric};
        input.type = KeyInput::Type::JapaneseTransliterate;
        input.kanaForm = forms[wParam - VK_F6];
        return true;
    }
    default:
        break;
    }
    if (IsCaretKey(wParam)) {
        if (!composing) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }

    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState)) return false;
    wchar_t buffer[8]{};
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(wParam), MAPVK_VK_TO_VSC);
    const int count = ToUnicodeEx(static_cast<UINT>(wParam), scanCode, keyboardState, buffer,
                                  static_cast<int>(std::size(buffer)), 0, GetKeyboardLayout(0));
    if (count != 1 || buffer[0] <= 0x20 || buffer[0] == 0x7F) return false;
    input.type = KeyInput::Type::Printable;
    input.character = buffer[0];
    return true;
}

HRESULT TextService::HandleJapaneseKey(ITfContext* context, TfEditCookie editCookie,
                                       const KeyInput& input) {
    switch (input.type) {
    case KeyInput::Type::Printable: {
        // After a conversion, typing on commits it and starts anew.
        if (japanese_.IsConverted()) {
            const HRESULT hr = CommitJapanese(context, editCookie);
            if (FAILED(hr)) return hr;
        }
        if (!japanese_.IsComposing()) CheckJapaneseContext(context, editCookie);
        const HRESULT hr = EnsureComposition(context, editCookie);
        if (FAILED(hr)) return hr;
        japanese_.Insert(input.character);
        return ShowJapanesePreedit(context, editCookie);
    }
    case KeyInput::Type::Backspace:
        japanese_.Backspace();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::Cancel:
        japanese_.Cancel();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseConvert:
        japanese_.Convert();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseCycleKana:
        japanese_.CycleKana();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseTransliterate:
        japanese_.Transliterate(input.kanaForm);
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseNextCandidate:
        japanese_.NextCandidate();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapanesePreviousCandidate:
        japanese_.PreviousCandidate();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseMoveFocus:
        japanese_.MoveFocus(input.delta);
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseResize:
        japanese_.ResizeFocus(input.delta);
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseSelectCandidate:
        japanese_.SelectCandidate(input.candidateIndex);
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::CandidateSelection:
        if (!japanese_.IsConverted()) {
            // A click on a prediction commits it.
            japanese_.ChoosePrediction(input.candidateIndex);
            return CommitJapanese(context, editCookie);
        }
        japanese_.SelectCandidate(input.candidateIndex);
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseNextPrediction:
        japanese_.NextPrediction();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapanesePreviousPrediction:
        japanese_.PreviousPrediction();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseClearPrediction:
        japanese_.ClearPredictionChoice();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseMoveCaret:
        japanese_.MoveCaret(input.delta);
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::JapaneseDelete:
        japanese_.Delete();
        return ShowJapanesePreedit(context, editCookie);
    case KeyInput::Type::Enter:
    case KeyInput::Type::JapaneseCommit:
    case KeyInput::Type::EndComposition: {
        const HRESULT hr = CommitJapanese(context, editCookie);
        // An English word left from before a switch ends as typed.
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        return hr;
    }
    case KeyInput::Type::InsertCharacter:
        return InsertAtSelection(context, editCookie, std::wstring(1, input.character));
    default:
        return S_FALSE;
    }
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
    const auto meaning = ProcessMeanings().Lookup(text, reading);
    if (!meaning) return {};
    return {meaning->headword, meaning->senses};
}

void TextService::MarkMeanings(std::vector<Candidate>& candidates, std::size_t first, std::size_t count,
                               std::wstring_view reading) const {
    if (!userSettings_.meaningsEnabled) return;
    const auto& meanings = ProcessMeanings();
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
    const auto& predictions = japanese_.Predictions();
    if (TraceEnabled()) {
        wchar_t line[96]{};
        swprintf_s(line, L"Japanese list converted=%d open=%d predictions=%zu", japanese_.IsConverted() ? 1 : 0,
                   japanese_.IsCandidateListOpen() ? 1 : 0, predictions.size());
        Trace(line);
    }
    if (!japanese_.IsConverted() && !predictions.empty() && userSettings_.candidateWindowEnabled) {
        std::vector<Candidate> rows;
        for (std::size_t i = 0; i < predictions.size(); ++i) {
            Candidate row;
            row.text = predictions[i].text;
            row.id = static_cast<std::uint32_t>(i + 1);
            rows.push_back(std::move(row));
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i].hasMeaning = userSettings_.meaningsEnabled &&
                                 ProcessMeanings().Lookup(predictions[i].text, predictions[i].reading).has_value();
        }
        const auto chosen = japanese_.ChosenPrediction();
        const RECT anchor = GetCandidateAnchor(context, editCookie);
        candidateAnchor_ = anchor;
        candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
        candidateWindow_.SetJapanese(userdata::UseJapaneseUi(userSettings_.uiLanguage));
        const CandidateDetail detail =
            chosen && *chosen < predictions.size()
                ? MeaningFor(predictions[*chosen].text, predictions[*chosen].reading)
                : CandidateDetail{};
        candidateWindow_.Show(anchor, rows, chosen ? *chosen : CandidateWindow::kNoSelection, 0, rows.size(),
                              detail);
        return;
    }
    const auto* candidates = japanese_.FocusedCandidates();
    if (!userSettings_.candidateWindowEnabled || !japanese_.IsCandidateListOpen() || !candidates ||
        candidates->empty()) {
        candidateWindow_.Hide();
        return;
    }
    const std::size_t selected = std::min(japanese_.FocusedSelection(), candidates->size() - 1);
    const std::size_t page = selected / japanesePage_ * japanesePage_;
    std::vector<Candidate> rows;
    rows.reserve(candidates->size());
    for (std::size_t i = 0; i < candidates->size(); ++i) {
        Candidate row;
        row.text = (*candidates)[i].text;
        row.id = static_cast<std::uint32_t>(i % japanesePage_ + 1);
        if ((*candidates)[i].slip || (*candidates)[i].spellingCorrection) row.label = SemanticLabel::Suggestion;
        rows.push_back(std::move(row));
    }
    MarkMeanings(rows, page, japanesePage_, japanese_.FocusedReading());
    const RECT anchor = JapaneseCandidateAnchor(context, editCookie);
    candidateAnchor_ = anchor;
    candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
    candidateWindow_.SetJapanese(userdata::UseJapaneseUi(userSettings_.uiLanguage));
    candidateWindow_.Show(anchor, rows, selected, page, std::min(japanesePage_, rows.size() - page),
                          MeaningFor((*candidates)[selected].text, japanese_.FocusedReading()));
}

HRESULT TextService::CommitJapanese(ITfContext* context, TfEditCookie editCookie) {
    candidateWindow_.Hide();
    if (!japanese_.IsComposing()) return EndComposition(editCookie);
    const std::wstring text = japanese_.Commit();
    lastJapaneseCommit_ = text;
    if (!composition_) {
        return text.empty() ? S_OK : InsertAtSelection(context, editCookie, text);
    }
    const HRESULT hr = ReplaceComposition(context, editCookie, text);
    if (FAILED(hr)) return hr;
    return EndComposition(editCookie);
}

}  // namespace tekito::tsf
