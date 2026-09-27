#pragma once

#include "Core/InputStateMachine.h"
#include "Core/Japanese/JapaneseComposer.h"

#include <msctf.h>
#include <windows.h>
#include <atomic>

namespace tekito::tsf {

class TextService;

struct KeyInput {
    enum class Type {
        Printable,
        Space,
        ShiftSpace,
        Tab,
        ShiftTab,
        UpArrow,
        DownArrow,
        CandidateSelection,
        Backspace,
        Punctuation,
        Cancel,
        Enter,
        EndComposition,
        // Not a key: move the candidate list to follow the composition
        // after the host's layout changed (scrolling, resizing).
        Reposition,
        // Japanese mode: Space or Henkan, Muhenkan, F6-F10.
        JapaneseConvert,
        JapaneseCycleKana,
        JapaneseTransliterate,
        // Japanese conversion: candidates (Up/Down, Shift+Space), phrases
        // (Left/Right by `delta`), phrase length (Shift+Left/Right), and a
        // candidate by its number (`candidateIndex`).
        JapaneseNextCandidate,
        JapanesePreviousCandidate,
        JapaneseMoveFocus,
        JapaneseResize,
        JapaneseSelectCandidate,
        // Japanese typing: the caret by `delta` kana, and Delete.
        JapaneseMoveCaret,
        JapaneseDelete,
        // Not a key: commit the Japanese text (the mode or focus changed).
        JapaneseCommit,
        // Types `character` outside a composition (a full-width space).
        InsertCharacter,
    } type;
    wchar_t character{0};
    japanese::KanaForm kanaForm{japanese::KanaForm::Hiragana};
    int delta{0};
    std::size_t candidateIndex{0};
    PunctuationRole punctuationRole{PunctuationRole::ClauseSeparator};
};

class KeyEditSession final : public ITfEditSession {
public:
    KeyEditSession(TextService* service, ITfContext* context, KeyInput input);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie editCookie) override;

private:
    ~KeyEditSession() = default;

    std::atomic<ULONG> refCount_{1};
    TextService* service_{nullptr};
    ITfContext* context_{nullptr};
    KeyInput input_{};
};

}  // namespace tekito::tsf
