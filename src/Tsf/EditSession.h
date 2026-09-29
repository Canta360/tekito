#pragma once

#include "Core/InputStateMachine.h"
#include "Core/Japanese/JapaneseComposer.h"

#include <msctf.h>
#include <windows.h>
#include <atomic>
#include <functional>
#include <utility>

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
        // Japanese predictions: Tab/Down, Up/Shift+Tab, and Esc to step out.
        JapaneseNextPrediction,
        JapanesePreviousPrediction,
        JapaneseClearPrediction,
        // An English word typed with Shift in Japanese: Enter commits it (no
        // new line); a letter without Shift after its space goes back to
        // Japanese and becomes `character`.
        EnglishSegmentCommit,
        EnglishSegmentEnd,
        // Not a key: commit the Japanese text (the mode or focus changed).
        JapaneseCommit,
        // Types `character` outside a composition (a full-width space).
        InsertCharacter,
        // Not a key: show the mode by the caret (the user switched modes).
        ShowModeIndicator,
        // English: `character` finishes a symbol's spelling or a sum before
        // the caret ("->", "(c)", "1+2="), which becomes the composition.
        SpecialEnd,
    } type;
    wchar_t character{0};
    japanese::KanaForm kanaForm{japanese::KanaForm::Hiragana};
    int delta{0};
    // The key belongs to an English word typed with Shift in Japanese.
    bool englishSegment{false};
    std::size_t candidateIndex{0};
    PunctuationRole punctuationRole{PunctuationRole::ClauseSeparator};
};

// Runs `read` in a read-only session, to look at the document.
class ReadEditSession final : public ITfEditSession {
public:
    explicit ReadEditSession(std::function<void(TfEditCookie)> read) : read_(std::move(read)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie editCookie) override;

private:
    ~ReadEditSession() = default;

    std::atomic<ULONG> refCount_{1};
    std::function<void(TfEditCookie)> read_;
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
