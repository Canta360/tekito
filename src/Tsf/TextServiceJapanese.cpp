// The ja-JP profile: switching among Japanese, English Auto and Direct, and
// typing Japanese. The English typing path is in TextService.cpp.

#include "Tsf/TextService.h"

#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/RomajiTable.h"
#include "Tsf/Compartments.h"
#include "Tsf/Diagnostics.h"
#include "Tsf/TekitoGuids.h"

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

}  // namespace

InputMode TextService::SharedMode() const noexcept {
    if (!runtimeMode_) return mode_;
    return japaneseProfile_ ? runtimeMode_->JapaneseMode() : runtimeMode_->Mode();
}

InputMode TextService::ToggledMode() const noexcept {
    if (!japaneseProfile_) {
        return mode_ == InputMode::Convert ? InputMode::Direct : InputMode::Convert;
    }
    if (mode_ != InputMode::Japanese) return InputMode::Japanese;
    return userSettings_.japaneseProfileEnglishMode == InputMode::Direct ? InputMode::Direct
                                                                         : InputMode::Convert;
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
    if (japanese) japanese_.SetTable(ProcessRomajiTable());
}

void TextService::SetJapaneseProfile(bool japanese) {
    if (japanese == japaneseProfile_) return;
    Trace(japanese ? L"Profile Japanese" : L"Profile English");
    if (japanese_.IsComposing() || state_.IsActive()) FinishCompositionLater();
    japaneseProfile_ = japanese;
    activationSettling_ = japanese;
    if (japanese) japanese_.SetTable(ProcessRomajiTable());
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
        key.mode = ToggledMode();
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

bool TextService::TranslateJapaneseKey(WPARAM wParam, KeyInput& input) {
    const bool composing = japanese_.IsComposing();
    if (HasCommandModifier()) {
        // Shortcuts act on the committed text.
        if (!composing) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
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
        if (composition_) {
            const HRESULT hr = ReplaceComposition(context, editCookie, std::wstring{});
            if (FAILED(hr)) return hr;
        }
        return EndComposition(editCookie);
    }
    const HRESULT hr = EnsureComposition(context, editCookie);
    if (FAILED(hr)) return hr;
    return ReplaceComposition(context, editCookie, japanese_.Preedit());
}

HRESULT TextService::CommitJapanese(ITfContext* context, TfEditCookie editCookie) {
    if (!japanese_.IsComposing()) return EndComposition(editCookie);
    const std::wstring text = japanese_.Commit();
    if (!composition_) {
        return text.empty() ? S_OK : InsertAtSelection(context, editCookie, text);
    }
    const HRESULT hr = ReplaceComposition(context, editCookie, text);
    if (FAILED(hr)) return hr;
    return EndComposition(editCookie);
}

}  // namespace tekito::tsf
