#pragma once

#include "Core/InputStateMachine.h"
#include "Core/Japanese/JapaneseKeys.h"

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
        // A key in Japanese mode: what it does is `japaneseKey`
        // (japanese::TranslateKey).
        JapaneseKey,
        // An English word typed with Shift in Japanese: Enter commits it (no
        // new line); a letter without Shift after its space goes back to
        // Japanese and becomes `character`.
        EnglishSegmentCommit,
        EnglishSegmentEnd,
        // Not a key: commit the Japanese text (the mode or focus changed).
        JapaneseCommit,
        // Not a key: show the mode by the caret (the user switched modes).
        ShowModeIndicator,
        // English: `character` finishes a symbol's spelling or a sum before
        // the caret ("->", "(c)", "1+2="), which becomes the composition.
        SpecialEnd,
    } type;
    wchar_t character{0};
    japanese::KeyCommand japaneseKey;
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
