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
        // Not a key: commit the Japanese text (the mode or focus changed).
        JapaneseCommit,
        // Types `character` outside a composition (a full-width space).
        InsertCharacter,
    } type;
    wchar_t character{0};
    japanese::KanaForm kanaForm{japanese::KanaForm::Hiragana};
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
