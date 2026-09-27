#pragma once

#include "Core/Japanese/JapaneseConverter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class JapaneseLearningStore;
class RomajiTable;

// How a phrase (or the whole text before conversion) is written.
enum class KanaForm : std::uint8_t {
    Hiragana,
    Katakana,
    HalfWidthKatakana,
    FullWidthAlphanumeric,  // the keys as typed, full-width (F9)
    HalfWidthAlphanumeric,  // the keys as typed (F10)
};

// Which marks the 、 and 。 keys write (UserSettings::japanesePunctuation).
enum class PunctuationStyle : std::uint8_t {
    ToutenKuten,   // 、。
    CommaPeriod,   // ，．
    CommaKuten,    // ，。
    ToutenPeriod,  // 、．
};

// A run of the text shown while composing, for underlining.
struct PreeditSegment {
    std::wstring text;
    bool converted{false};
    bool focused{false};
};

// The text being typed in Japanese mode, before it is committed.
//
// Keys go through the romaji table as they are typed; keys that may still
// become kana ("k", "n", "ky") stay pending. Space converts the text into
// phrases with the converter (without one, between hiragana and katakana);
// Space again steps through the focused phrase's candidates. F6-F10 and
// Muhenkan write the focused phrase (or, before conversion, the whole text)
// in another form. After any of those, the next key the caller sees commits
// the text, as in Microsoft IME.
class JapaneseComposer final {
public:
    explicit JapaneseComposer(const RomajiTable* table = nullptr) noexcept : table_(table) {}

    void SetTable(const RomajiTable* table) noexcept;
    void SetConverter(const JapaneseConverter* converter) noexcept { converter_ = converter; }
    // What the user chose before puts candidates first; each commit is
    // recorded. nullptr turns learning off.
    void SetLearning(JapaneseLearningStore* learning) noexcept { learning_ = learning; }
    void SetPunctuationStyle(PunctuationStyle style) noexcept { punctuation_ = style; }
    // Hiragana or Katakana: how new text is shown while typing.
    void SetInputForm(KanaForm form) noexcept;
    [[nodiscard]] KanaForm InputForm() const noexcept { return inputForm_; }

    [[nodiscard]] bool IsComposing() const noexcept { return !units_.empty() || !pending_.empty(); }
    // True once converted (Space, Muhenkan, F6-F10): typing on commits.
    [[nodiscard]] bool IsConverted() const noexcept { return !phrases_.empty(); }

    // Types a key at the caret.
    void Insert(wchar_t key);
    // Before conversion: removes the key or kana before the caret. After:
    // back to kana.
    void Backspace();
    // Before conversion: removes the kana after the caret.
    void Delete();
    // Before conversion: moves the caret by kana (Left, Right; Home and End
    // move by a large amount).
    void MoveCaret(int delta);
    // Where the caret is in Preedit(), in characters.
    [[nodiscard]] std::size_t CaretOffset() const;
    // Back to the typed kana after a conversion; otherwise drops everything.
    void Cancel();
    // Space and Henkan: converts, then steps through the candidates.
    void Convert();
    void NextCandidate();
    void PreviousCandidate();
    void SelectCandidate(std::size_t index);
    // Left and Right: which phrase the candidates are for.
    void MoveFocus(int delta);
    // Shift+Left and Shift+Right: the focused phrase takes one character
    // less or more, and what follows is converted again.
    void ResizeFocus(int delta);
    // Muhenkan: hiragana -> katakana -> half-width katakana -> hiragana.
    void CycleKana();
    // F6-F10. Pressing F9 or F10 again changes the letter case.
    void Transliterate(KanaForm form);

    // What to show in the document while composing.
    [[nodiscard]] std::wstring Preedit() const;
    [[nodiscard]] std::vector<PreeditSegment> Segments() const;
    // The focused phrase's candidates, and whether the list is open (from
    // the second Space, or an arrow key).
    [[nodiscard]] const std::vector<PhraseCandidate>* FocusedCandidates() const noexcept;
    [[nodiscard]] std::size_t FocusedSelection() const noexcept;
    [[nodiscard]] bool IsCandidateListOpen() const noexcept { return listOpen_ && IsConverted(); }

    // The text to commit (pending keys are resolved first); clears the
    // composition.
    [[nodiscard]] std::wstring Commit();
    void Clear() noexcept;

private:
    struct Unit {
        std::wstring keys;  // what was typed
        std::wstring kana;  // what the table wrote for it
    };
    struct PhraseState {
        std::size_t begin{0};
        std::size_t length{0};
        std::vector<PhraseCandidate> candidates;
        std::size_t selected{0};
        // Set by F6-F10 and Muhenkan; wins over the selected candidate.
        std::optional<KanaForm> form;
        // F9/F10 pressed again: 0 as typed, 1 upper case, 2 capitalized,
        // 3 lower case.
        int letterCase{0};
    };

    void Feed(wchar_t key);
    // Splits the unit around reading position `offset` so a unit boundary
    // falls there; returns the index of the unit that starts there.
    std::size_t SplitAt(std::size_t offset);
    [[nodiscard]] std::size_t ReadingOffset(std::size_t units) const;
    void FlushOnce();
    void FlushAll();
    void Emit(std::wstring keys, std::wstring kana);
    // The typed kana as the converter reads it.
    [[nodiscard]] std::wstring Reading() const;
    [[nodiscard]] std::wstring Keys(bool includePending) const;
    // The keys typed for the reading from `begin`, `length` long.
    [[nodiscard]] std::wstring KeysFor(std::size_t begin, std::size_t length) const;
    [[nodiscard]] std::wstring ApplyPunctuation(std::wstring text) const;
    [[nodiscard]] std::wstring RenderTyping(bool includePending) const;
    [[nodiscard]] std::wstring PhraseText(const PhraseState& phrase) const;
    [[nodiscard]] std::wstring FormText(KanaForm form, std::size_t begin, std::size_t length,
                                        int letterCase) const;
    // Phrases from the converter, or one phrase for the whole text.
    void BuildPhrases(bool convert);
    [[nodiscard]] std::vector<PhraseCandidate> KanaCandidates(std::wstring_view reading) const;
    void AddPhrases(std::vector<Phrase> phrases, const std::wstring& reading);

    const RomajiTable* table_{nullptr};
    const JapaneseConverter* converter_{nullptr};
    JapaneseLearningStore* learning_{nullptr};
    std::vector<Unit> units_;
    // Units before the caret; pending keys belong at the caret.
    std::size_t caret_{0};
    std::wstring pending_;
    KanaForm inputForm_{KanaForm::Hiragana};
    PunctuationStyle punctuation_{PunctuationStyle::ToutenKuten};
    std::vector<PhraseState> phrases_;
    std::size_t focus_{0};
    bool listOpen_{false};
};

}  // namespace tekito::japanese
