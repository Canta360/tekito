#pragma once

#include "Core/Japanese/JapaneseComposer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tekito::japanese {

// What each key does while typing Japanese, apart from the text service:
// the TSF text service and the testbed both turn a key into a KeyCommand
// with TranslateKey, carry it out with ApplyKey, and show CandidateListFor,
// so they behave alike. They only write the result where it goes.

// A key pressed in Japanese mode.
struct KeyPress {
    enum class Key : std::uint8_t {
        Other,      // anything not below: the application's
        Character,  // a key that types `character`
        Space,
        Convert,     // Henkan
        NonConvert,  // Muhenkan
        Enter,
        Backspace,
        Escape,
        Tab,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        Delete,
        Insert,
        F6,
        F7,
        F8,
        F9,
        F10,
    };
    Key key{Key::Other};
    // Character: what the key types.
    wchar_t character{0};
    // 1-9 for those digit keys (the top row or the number pad), else 0:
    // with the candidate list open they choose from it.
    int digit{0};
    bool shift{false};
    // Ctrl, Alt or Windows held: a shortcut for the application.
    bool command{false};
};

// What a key does to the composition.
struct KeyCommand {
    enum class Action : std::uint8_t {
        Insert,             // types `character`
        Backspace,
        Cancel,             // Esc: back to kana, or drop what was typed
        Convert,            // Space, Henkan: convert, then the next candidate
        CycleKana,          // Muhenkan
        Transliterate,      // F6-F10, to `form`
        NextCandidate,
        PreviousCandidate,
        MoveFocus,          // to the phrase `delta` away
        Resize,             // the focused phrase by `delta`
        SelectCandidate,    // `index` in the focused phrase's list
        ChooseRow,          // a click on row `index` of the list shown
        NextPrediction,
        PreviousPrediction,
        ClearPrediction,
        MoveCaret,          // by `delta` kana before conversion
        Delete,
        Commit,             // Enter, or anything that ends the composition
        InsertOutside,      // types `character` with no composition (a full-width space)
    };
    Action action{Action::Commit};
    wchar_t character{0};
    int delta{0};
    std::size_t index{0};
    KanaForm form{KanaForm::Hiragana};
    // Commit only: the key still goes on to the application afterwards
    // (a shortcut, an arrow that leaves the text).
    bool letKeyThrough{false};
};

struct KeyOptions {
    // Space outside a composition types a full-width space (Shift the other).
    bool fullWidthSpace{true};
    // Rows of the candidate list a page shows (1-9 choose from it).
    std::size_t pageSize{9};
};

// What `key` does now; nothing when it is the application's.
[[nodiscard]] std::optional<KeyCommand> TranslateKey(const JapaneseComposer& composer, const KeyPress& key,
                                                     const KeyOptions& options);

// What carrying out a command produced besides the composition itself.
struct KeyOutcome {
    // Text committed: it goes before the composition, which ends there.
    std::optional<std::wstring> committed;
    // Text typed with no composition.
    std::wstring outside;
};

KeyOutcome ApplyKey(JapaneseComposer& composer, const KeyCommand& command);

// The list to show beside the composition: the predictions while typing,
// or the focused phrase's candidates once its list is open.
struct CandidateList {
    enum class Kind : std::uint8_t { None, Predictions, Candidates };
    struct Row {
        std::wstring text;
        // The reading it was converted from, for its meaning.
        std::wstring reading;
        // The number shown beside it (1-9 on each page).
        std::uint32_t number{0};
        // Read with a slip undone, or a spelling Mozc corrects.
        bool suggestion{false};
    };
    Kind kind{Kind::None};
    std::vector<Row> rows;
    std::optional<std::size_t> selected;
    // The page shown: `count` rows from `pageStart`.
    std::size_t pageStart{0};
    std::size_t count{0};
};

[[nodiscard]] CandidateList CandidateListFor(const JapaneseComposer& composer, std::size_t pageSize);

}  // namespace tekito::japanese
