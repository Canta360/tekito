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

bool IsModifierKey(WPARAM key) {
    switch (key) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
        return true;
    default:
        return false;
    }
}

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
    return AvailableMode(runtimeMode_->JapaneseMode());
}

InputMode TextService::AvailableMode(InputMode mode) const noexcept {
    if (mode == InputMode::Japanese && !JapaneseModeAvailable()) {
        return MixedModeAvailable() ? InputMode::Mixed : InputMode::Convert;
    }
    if (mode == InputMode::Mixed && !MixedModeAvailable()) {
        return JapaneseModeAvailable() ? InputMode::Japanese : InputMode::Convert;
    }
    return mode;
}

InputMode TextService::ToggledMode() const noexcept {
    if (!JapaneseTypingAvailable()) {
        return mode_ == InputMode::Convert ? InputMode::Direct : InputMode::Convert;
    }
    // The modes chosen to go round (UserSettings::switchModes) that are on,
    // in the order chosen (UserSettings::switchOrder).
    std::vector<InputMode> round;
    for (const auto& [mode, bit] : userdata::SwitchOrderModes(userSettings_.switchOrder)) {
        if ((userSettings_.switchModes & bit) != 0 && AvailableMode(mode) == mode) round.push_back(mode);
    }
    // Fewer than two: a typing mode and Auto.
    if (round.size() < 2) {
        const auto typing = AvailableMode(InputMode::Japanese);
        return mode_ == typing ? InputMode::Convert : typing;
    }
    const auto at = std::find(round.begin(), round.end(), mode_);
    return at == round.end() || at + 1 == round.end() ? round.front() : *(at + 1);
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

void TextService::HideJapaneseWord(const std::wstring& reading, const std::wstring& surface) {
    japanese::UserWord word{reading, surface, japanese::UserWordKind::Noun, japanese::UserWordAction::Suppress};
    if (std::find(japaneseUserWords_.begin(), japaneseUserWords_.end(), word) != japaneseUserWords_.end()) return;
    japaneseUserWords_.push_back(std::move(word));
    RefreshJapaneseUserWords();
    // Kept with the user's words (Settings lists it as not shown), and the
    // other apps typing with TEKITO read them again.
    if (settingsRepository_ && settingsRepository_->SaveJapaneseUserWords(japaneseUserWords_) && runtimeMode_) {
        runtimeMode_->NotifyDictionaryChanged();
    }
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
        if (TypesJapanese(mode_)) {
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
    if (!JapaneseTypingAvailable()) {
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
            target = TypesJapanese(mode_) ? mode_ : InputMode::Japanese;
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
    if (!JapaneseTypingAvailable()) {
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
        if (japanese_.IsComposing() || TypesJapanese(mode_)) return false;
        key.mode = InputMode::Japanese;
        return true;
    case VK_NONCONVERT:
        if (japanese_.IsComposing()) return false;
        // To English; pressed again in English, between Auto and Direct.
        key.mode = TypesJapanese(mode_) ? english
                   : mode_ == InputMode::Convert ? InputMode::Direct
                                                 : InputMode::Convert;
        return true;
    case kVkHiragana:
    case kVkKatakana:
        key.mode = TypesJapanese(mode_) ? mode_ : InputMode::Japanese;
        key.setsInputForm = true;
        key.inputForm = wParam == kVkKatakana || KeyDown(VK_SHIFT) ? japanese::KanaForm::Katakana
                                                                   : japanese::KanaForm::Hiragana;
        return true;
    case kVkImeOn:
        key.mode = TypesJapanese(mode_) ? mode_ : InputMode::Japanese;
        return true;
    case kVkImeOff:
        key.mode = english;
        return true;
    default:
        return false;
    }
}

std::optional<InputMode> TextService::AltMode(bool right) const {
    switch (std::clamp(right ? userSettings_.altRightMode : userSettings_.altLeftMode, 0, 4)) {
    case 0: return InputMode::Japanese;
    case 1: return InputMode::Mixed;
    case 2: return InputMode::Convert;
    case 3: return InputMode::Direct;
    default: return std::nullopt;
    }
}

TextService::ModeKey TextService::AltModeKey(bool right) const {
    ModeKey key;
    key.mode = mode_;
    const auto wanted = AltMode(right);
    if (!wanted) return key;
    // The mode chosen for it, or the nearest one there is (Japanese for
    // Mix when Mix is off; Auto when there is no Japanese).
    key.mode = AvailableMode(*wanted);
    if (TypesJapanese(*wanted) && !JapaneseTypingAvailable()) key.mode = InputMode::Convert;
    return key;
}

namespace {
// The text service whose Alt hook runs on this thread.
thread_local TextService* altHookOwner = nullptr;
}  // namespace

void TextService::UpdateAltHook(bool on) {
    const bool wanted = on && clientId_ != TF_CLIENTID_NULL;
    if (wanted && !altHook_) {
        altHookOwner = this;
        // The one keyboard hook (scripts/verify.ps1 allows it by this mark):
        // this thread only, while the left and right Alt switch is on; it
        // keeps no key. verify-allow
        altHook_ = SetWindowsHookExW(WH_KEYBOARD, &TextService::AltHookProc, nullptr, GetCurrentThreadId());
        if (!altHook_) altHookOwner = nullptr;
    } else if (!wanted && altHook_) {
        UnhookWindowsHookEx(altHook_);
        altHook_ = nullptr;
        if (altHookOwner == this) altHookOwner = nullptr;
        if (altTimer_) KillTimer(nullptr, altTimer_);
        altTimer_ = 0;
        altTap_.reset();
        altTapped_.reset();
    }
}

LRESULT CALLBACK TextService::AltHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && altHookOwner && altHookOwner->OnAltHookKey(wParam, lParam)) return 1;
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool TextService::OnAltHookKey(WPARAM key, LPARAM flags) {
    constexpr ULONGLONG kTapMs = 600;
    const bool up = (static_cast<ULONG_PTR>(flags) & 0x80000000u) != 0;
    const bool alt = key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
    if (!alt) {
        // Any other key pressed with it: Alt is for a shortcut.
        if (!up) altTap_.reset();
        return false;
    }
    const bool right = key == VK_RMENU || (flags & (1 << 24)) != 0;
    if (!up) {
        // Held down: as it was.
        if ((flags & (1 << 30)) != 0) return false;
        // Not with Ctrl (AltGr is Ctrl and the right Alt), Shift or Windows.
        const bool others = KeyDown(VK_CONTROL) || KeyDown(VK_SHIFT) || KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
        if (userSettings_.altSwitches && AltMode(right) && !others && !ProcessPassesThrough()) {
            altTap_ = right;
            altDownAt_ = GetTickCount64();
        } else {
            altTap_.reset();
        }
        return false;
    }
    const bool tapped = altTap_ && *altTap_ == right && GetTickCount64() - altDownAt_ <= kTapMs;
    altTap_.reset();
    if (!tapped || !userSettings_.altSwitches) return false;
    if (TraceEnabled()) Trace(right ? L"Alt tapped (right)" : L"Alt tapped (left)");
    // Taken from the application, so that its menu does not open; its
    // keyboard state lets go of Alt all the same.
    BYTE state[256]{};
    if (GetKeyboardState(state)) {
        state[VK_MENU] &= 0x7F;
        state[VK_LMENU] &= 0x7F;
        state[VK_RMENU] &= 0x7F;
        SetKeyboardState(state);
    }
    // Switched once the hook has returned (committing may edit the text).
    altTapped_ = right;
    altTimer_ = SetTimer(nullptr, altTimer_, 0, &TextService::OnAltTapTimer);
    return true;
}

void CALLBACK TextService::OnAltTapTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    TextService* self = altHookOwner;
    if (!self || id != self->altTimer_) return;
    self->altTimer_ = 0;
    const auto right = std::exchange(self->altTapped_, std::nullopt);
    if (!right) return;
    self->SyncRuntimeState();
    self->ApplyModeKey(self->sinkContext_.Get(), self->AltModeKey(*right));
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
    // Mix takes capitals into its chunks itself (I, GitHub).
    if (mode_ == InputMode::Mixed && !englishSegment_) return false;
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
    if (TypesJapanese(mode_)) state_.SetInputMode(InputMode::Direct);
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
    // Mix: Enter after an English sentence ends it with a period, as English
    // Auto does, when Settings has the period on Enter.
    if (mode_ == InputMode::Mixed && wParam == VK_RETURN && userSettings_.periodOnEnter &&
        japanese_.EnglishSentenceOpen() && !HasCommandModifier()) {
        input.type = KeyInput::Type::MixedEnterPeriod;
        return true;
    }
    // The numeric keypad types its digits and marks as they are, half-width:
    // the application gets the key, after what is being typed is committed.
    // With the candidate list open its digits still choose.
    const bool keypad = (wParam >= VK_NUMPAD0 && wParam <= VK_DIVIDE) && wParam != VK_SEPARATOR;
    if (userSettings_.japaneseNumpadDirect && keypad && !HasCommandModifier() && !japanese_.IsCandidateListOpen()) {
        if (!japanese_.IsComposing()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }
    japanese::KeyOptions options = userdata::JapaneseKeyOptions(userSettings_, mode_ == InputMode::Mixed);
    options.pageSize = japanesePage_;
    const auto command = japanese::TranslateKey(
        japanese_,
        JapaneseKeyPressFor(wParam, userSettings_.japaneseIgnoreCapsLock,
                            userSettings_.japaneseKanaInput && mode_ == InputMode::Japanese),
        options);
    if (!command) {
        // Any other key ends the chance to take the last commit back.
        if (!IsModifierKey(wParam)) japanese_.ForgetCommit();
        return false;
    }
    // A key that only ends the text goes on to the application (LetsKeyThrough).
    input.type = command->letKeyThrough ? KeyInput::Type::EndComposition : KeyInput::Type::JapaneseKey;
    input.japaneseKey = *command;
    return true;
}

HRESULT TextService::HandleJapaneseKey(ITfContext* context, TfEditCookie editCookie,
                                       const KeyInput& input) {
    using Action = japanese::KeyCommand::Action;
    // Mix spells English as English Auto does, once its data is loaded.
    if (mode_ == InputMode::Mixed) (void)EnsureEngine();
    japanese::KeyCommand command;
    switch (input.type) {
    case KeyInput::Type::MixedEnterPeriod:
        return PutEnglishPeriod(context, editCookie);
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
    if (command.action == Action::UndoCommit) return UndoJapaneseCommit(context, editCookie);
    if (command.action == Action::Reconvert) return ReconvertJapanese(context, editCookie);
    if (command.action == Action::Commit) {
        const HRESULT hr = CommitJapanese(context, editCookie);
        // An English word left from before a switch ends as typed.
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        return hr;
    }
    // An opening bracket with its closing one, the caret between; what was
    // being typed is committed first.
    if (command.action == Action::Insert && userSettings_.bracketPairs) {
        if (const auto pair = japanese_.BracketPair(command.character)) {
            if (japanese_.IsComposing()) {
                const HRESULT hr = CommitJapanese(context, editCookie);
                if (FAILED(hr)) return hr;
            }
            candidateWindow_.Hide();
            return InsertPairAtSelection(context, editCookie, pair->first, pair->second);
        }
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
    if (TraceEnabled()) {
        wchar_t line[96]{};
        swprintf_s(line, L"Japanese key action=%d delta=%d converted=%d", static_cast<int>(command.action), command.delta,
                   japanese_.IsConverted() ? 1 : 0);
        Trace(line);
    }
    const std::wstring readingBefore = japanese_.ReportReading();
    const auto outcome = japanese::ApplyKey(japanese_, command);
    if (outcome.hidden) HideJapaneseWord(outcome.hidden->first, outcome.hidden->second);
    if (!outcome.outside.empty()) return InsertAtSelection(context, editCookie, outcome.outside);
    if (outcome.committed) {
        // A prediction chosen with a click.
        candidateWindow_.Hide();
        // Shift+Enter: the rest stays to convert, in a composition of its own.
        const bool rest = japanese_.IsComposing();
        if (!rest) NoteConversion(readingBefore, *outcome.committed);
        const HRESULT hr = WriteJapaneseCommit(context, editCookie, *outcome.committed);
        if (FAILED(hr) || !rest) return hr;
        const HRESULT started = EnsureComposition(context, editCookie);
        if (FAILED(started)) return started;
    }
    return ShowJapanesePreedit(context, editCookie);
}

namespace {
// How long typing stops before live conversion converts the text again.
constexpr UINT kRefinePauseMs = 200;
// The text service whose pause timer runs on this thread.
thread_local TextService* refineOwner = nullptr;
}  // namespace

namespace {
// The text service whose redisplay timer runs on this thread.
thread_local TextService* redisplayOwner = nullptr;
}  // namespace

void TextService::ScheduleJapaneseRedisplay() {
    const auto next = japanese_.IsComposing() ? japanese_.NextDisplayChange() : std::nullopt;
    if (!next) {
        if (redisplayTimer_) KillTimer(nullptr, redisplayTimer_);
        redisplayTimer_ = 0;
        return;
    }
    redisplayOwner = this;
    redisplayTimer_ = SetTimer(nullptr, redisplayTimer_, std::max<UINT>(*next, USER_TIMER_MINIMUM), &TextService::OnJapaneseRedisplayTimer);
}

void CALLBACK TextService::OnJapaneseRedisplayTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    TextService* self = redisplayOwner;
    if (!self || id != self->redisplayTimer_) return;
    self->redisplayTimer_ = 0;
    if (!self->compositionContext_) return;
    KeyInput input{};
    input.type = KeyInput::Type::JapaneseRedisplay;
    auto* session = new (std::nothrow) KeyEditSession(self, self->compositionContext_, input);
    if (!session) return;
    HRESULT sessionResult = E_FAIL;
    (void)self->compositionContext_->RequestEditSession(self->clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                                         &sessionResult);
    session->Release();
}

void TextService::ScheduleJapaneseRefine() {
    refineOwner = this;
    // SetTimer with the same id restarts the wait.
    refineTimer_ = SetTimer(nullptr, refineTimer_, kRefinePauseMs, &TextService::OnJapaneseRefineTimer);
}

void TextService::CancelJapaneseRefine() {
    if (refineTimer_) KillTimer(nullptr, refineTimer_);
    refineTimer_ = 0;
    if (refineOwner == this) refineOwner = nullptr;
}

namespace {
// How often the timer looks for the AI's answer.
constexpr UINT kAiLookMs = 30;
thread_local TextService* aiOwner = nullptr;
}  // namespace

void TextService::WatchAiAnswer() {
    if (aiTimer_ && aiOwner == this) return;
    aiOwner = this;
    aiTimer_ = SetTimer(nullptr, aiTimer_, kAiLookMs, &TextService::OnAiAnswerTimer);
}

void TextService::StopWatchingAiAnswer() {
    if (aiTimer_) KillTimer(nullptr, aiTimer_);
    aiTimer_ = 0;
    if (aiOwner == this) aiOwner = nullptr;
}

void CALLBACK TextService::OnAiAnswerTimer(HWND, UINT, UINT_PTR id, DWORD) {
    TextService* self = aiOwner;
    if (!self || id != self->aiTimer_) {
        KillTimer(nullptr, id);
        return;
    }
    if (!self->japanese_.AiAnswerCame()) return;
    self->StopWatchingAiAnswer();
    if (!self->compositionContext_) return;
    KeyInput input{};
    input.type = KeyInput::Type::JapaneseAiAnswer;
    auto* session = new (std::nothrow) KeyEditSession(self, self->compositionContext_, input);
    if (!session) return;
    HRESULT sessionResult = E_FAIL;
    (void)self->compositionContext_->RequestEditSession(self->clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                                         &sessionResult);
    session->Release();
}

void CALLBACK TextService::OnJapaneseRefineTimer(HWND, UINT, UINT_PTR id, DWORD) {
    TextService* self = refineOwner;
    if (!self || id != self->refineTimer_) {
        KillTimer(nullptr, id);
        return;
    }
    self->CancelJapaneseRefine();
    if (!self->compositionContext_ || !self->japanese_.RefinesOnPause()) return;
    KeyInput input{};
    input.type = KeyInput::Type::JapaneseRefine;
    auto* session = new (std::nothrow) KeyEditSession(self, self->compositionContext_, input);
    if (!session) return;
    HRESULT sessionResult = E_FAIL;
    (void)self->compositionContext_->RequestEditSession(self->clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                                         &sessionResult);
    session->Release();
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
    ScheduleJapaneseRedisplay();
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
    compositionText_ = text;
    // Its length only: the log never holds what is typed.
    if (TraceEnabled()) Trace((L"Japanese composition length=" + std::to_wstring(text.size())).c_str());

    LONG offset = 0;
    for (const auto& segment : segments) {
        const auto length = static_cast<LONG>(segment.text.size());
        ComPtr<ITfRange> part;
        LONG moved = 0;
        if (length > 0 && SUCCEEDED(range->Clone(part.Put())) &&
            SUCCEEDED(part->Collapse(editCookie, TF_ANCHOR_START)) &&
            SUCCEEDED(part->ShiftEnd(editCookie, offset + length, &moved, nullptr)) &&
            SUCCEEDED(part->ShiftStart(editCookie, offset, &moved, nullptr))) {
            const bool mix = mode_ == InputMode::Mixed && userSettings_.modeUnderlineColors;
            // The held sentence: marked, or (mixSettledMarker off) underlined
            // in light grey.
            const bool marked = segment.settled && userSettings_.mixSettledMarker;
            auto underline = segment.pending     ? JapaneseUnderline::Pending
                             : marked              ? JapaneseUnderline::Settled
                             : segment.settled  ? JapaneseUnderline::Held
                             : !segment.converted ? (mix ? JapaneseUnderline::MixInput : JapaneseUnderline::Input)
                             : segment.focused  ? (mix ? JapaneseUnderline::MixFocused : JapaneseUnderline::Focused)
                                                : (mix ? JapaneseUnderline::MixConverted : JapaneseUnderline::Converted);
            // What Mix took as English, in English Auto's colour.
            if (segment.english && mode_ == InputMode::Mixed && userSettings_.mixEnglishUnderline) {
                underline = marked            ? JapaneseUnderline::SettledEnglish
                            : segment.focused ? JapaneseUnderline::MixEnglishFocused
                                              : JapaneseUnderline::MixEnglish;
            }
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

japanese::MeaningLanguage TextService::MeaningLanguage() const {
    if (!userSettings_.meaningsInUiLanguage) return japanese::MeaningLanguage::Word;
    return userdata::UseJapaneseUi(userSettings_.uiLanguage) ? japanese::MeaningLanguage::Japanese
                                                             : japanese::MeaningLanguage::English;
}

namespace {
// What form of `base` an English word is, in the language of Settings
// (english-forms' kinds).
std::wstring FormLabel(const std::wstring& form, const std::wstring& base, bool japanese) {
    struct Label {
        std::wstring_view form;
        std::wstring_view japanese;
        std::wstring_view english;
    };
    static constexpr Label kLabels[] = {
        {L"past", L"\u306e\u904e\u53bb\u5f62", L"past tense of "},
        {L"past-participle", L"\u306e\u904e\u53bb\u5206\u8a5e", L"past participle of "},
        {L"past+past-participle", L"\u306e\u904e\u53bb\u5f62\u30fb\u904e\u53bb\u5206\u8a5e",
         L"past tense and past participle of "},
        {L"present-participle", L"\u306e\u73fe\u5728\u5206\u8a5e\u30fb\u52d5\u540d\u8a5e", L"-ing form of "},
        {L"third-person", L"\u306e\u4e09\u4eba\u79f0\u5358\u6570\u73fe\u5728\u5f62", L"third person singular of "},
        {L"plural", L"\u306e\u8907\u6570\u5f62", L"plural of "},
        {L"plural+third-person", L"\u306e\u8907\u6570\u5f62\u30fb\u4e09\u4eba\u79f0\u5358\u6570\u73fe\u5728\u5f62",
         L"plural or third person singular of "},
        {L"comparative", L"\u306e\u6bd4\u8f03\u7d1a", L"comparative of "},
        {L"superlative", L"\u306e\u6700\u4e0a\u7d1a", L"superlative of "},
    };
    const auto* label = std::find_if(std::begin(kLabels), std::end(kLabels), [&](const Label& l) { return l.form == form; });
    // Any other form: "a form of".
    if (label == std::end(kLabels)) {
        return japanese ? base + L" \u306e\u5909\u5316\u5f62" : L"a form of " + base;
    }
    return japanese ? base + L" " + std::wstring(label->japanese) : std::wstring(label->english) + base;
}
}  // namespace

CandidateDetail TextService::MeaningFor(std::wstring_view text, std::wstring_view reading) const {
    if (!userSettings_.meaningsEnabled || text.empty()) return {};
    const auto meaning = japanese::ProcessMeanings().Lookup(text, reading, MeaningLanguage());
    if (!meaning) return {};
    CandidateDetail detail{meaning->headword, meaning->senses};
    // Another form of an English word (went, called): which, after it.
    if (!meaning->base.empty()) {
        detail.form = FormLabel(meaning->form, meaning->base, userdata::UseJapaneseUi(userSettings_.uiLanguage));
    }
    return detail;
}

void TextService::MarkMeanings(std::vector<Candidate>& candidates, std::size_t first, std::size_t count,
                               std::wstring_view reading) const {
    if (!userSettings_.meaningsEnabled) return;
    const auto& meanings = japanese::ProcessMeanings();
    const auto language = MeaningLanguage();
    for (std::size_t i = first; i < candidates.size() && i < first + count; ++i) {
        candidates[i].hasMeaning = meanings.Lookup(candidates[i].text, reading, language).has_value();
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
    // The keys typed, small above the list, or alone (UserSettings::showTypedKeys).
    const std::wstring caption = userSettings_.showTypedKeys && japanese_.IsComposing() ? japanese_.TypedKeys() : std::wstring{};
    if (!userSettings_.candidateWindowEnabled || (list.kind == Kind::None && caption.empty())) {
        candidateWindow_.Hide();
        return;
    }
    if (list.kind == Kind::None) {
        candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
        candidateWindow_.SetZoom(userSettings_.candidateWindowScale);
        candidateWindow_.Show(JapaneseCandidateAnchor(context, editCookie), {}, CandidateWindow::kNoSelection, 0, 0, {},
                              caption);
        return;
    }
    std::vector<Candidate> rows;
    rows.reserve(list.rows.size());
    for (std::size_t i = 0; i < list.rows.size(); ++i) {
        Candidate row;
        row.text = list.rows[i].text;
        row.id = list.rows[i].number;
        if (list.rows[i].suggestion) row.label = SemanticLabel::Suggestion;
        row.groupStart = list.rows[i].groupStart;
        // Only the rows on the page shown are looked up.
        row.hasMeaning = userSettings_.meaningsEnabled && i >= list.pageStart && i < list.pageStart + list.count &&
                         japanese::ProcessMeanings()
                             .Lookup(list.rows[i].text, list.rows[i].reading, MeaningLanguage())
                             .has_value();
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
    candidateWindow_.SetZoom(userSettings_.candidateWindowScale);
    candidateWindow_.SetRainbow(userSettings_.rainbowAccent);
    candidateWindow_.SetJapanese(userdata::UseJapaneseUi(userSettings_.uiLanguage));
    candidateWindow_.Show(anchor, rows, selected ? *selected : CandidateWindow::kNoSelection, list.pageStart,
                          list.count, detail, caption);
}

HRESULT TextService::CommitJapanese(ITfContext* context, TfEditCookie editCookie) {
    candidateWindow_.Hide();
    if (!japanese_.IsComposing()) return EndComposition(editCookie);
    std::wstring reading = japanese_.ReportReading();
    std::wstring text = japanese_.Commit();
    NoteConversion(std::move(reading), text);
    return WriteJapaneseCommit(context, editCookie, text);
}

HRESULT TextService::WriteJapaneseCommit(ITfContext* context, TfEditCookie editCookie, const std::wstring& committed) {
    std::wstring text = committed;
    // Mix: Japanese after English takes back the space typed after it,
    // unless Settings spaces English from Japanese.
    const bool takeSpace = japanese_.TakeSpaceBefore();
    if (userdata::SpacesEnglish(userSettings_, mode_ == InputMode::Mixed) && !text.empty()) {
        text = japanese::JapaneseComposer::SpacedEnglish(CommitContext(context, editCookie), text);
    } else if (takeSpace && !text.empty()) {
        TakeBackSpaceBefore(context, editCookie);
    }
    lastJapaneseCommit_ = text;
    if (!composition_) {
        return text.empty() ? S_OK : InsertAtSelection(context, editCookie, text);
    }
    const HRESULT hr = ReplaceComposition(context, editCookie, text);
    if (FAILED(hr)) return hr;
    return EndComposition(editCookie);
}

}  // namespace tekito::tsf
