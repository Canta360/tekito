#pragma once

#include "Core/Japanese/JapaneseConverter.h"
#include "Core/SpecialConversions.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tekito::japanese {

class JapaneseLearningStore;
class KeyConverter;
class JapaneseUserDictionary;
class Loanwords;
class PostalCodes;
class RomajiTable;

// One romaji unit: the keys it took and the kana it made. Keys the table
// cannot read come out as they are, as leftovers.
struct RomajiToken {
    std::size_t keys{0};
    std::wstring kana;
    bool leftover{false};
};

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
    // Converts the keys as typed, correcting slips in them (tried first on
    // Space; nullptr leaves conversion to the kana as typed).
    void SetKeyConverter(const KeyConverter* keyConverter) noexcept { keyConverter_ = keyConverter; }
    // English words for katakana candidates (ミーティング -> meeting), offered
    // right after them; nullptr offers none.
    void SetLoanwords(const Loanwords* loanwords) noexcept { loanwords_ = loanwords; }
    // The user's words, for conversion and predictions; null for none.
    void SetUserDictionary(const JapaneseUserDictionary* user) noexcept { userDictionary_ = user; }
    // Dates for "きょう", numbers in kanji, symbols by reading and sums for
    // "1+2=", as far as `options` has them on; null for none.
    void SetSpecialConversions(const SpecialConversions* special, SpecialConversionOptions options = {}) noexcept {
        special_ = special;
        specialOptions_ = options;
    }
    // The addresses a postal code covers, offered for 1000001 or 100-0001
    // along with the numbers; null for none.
    void SetPostalCodes(const PostalCodes* postalCodes) noexcept { postalCodes_ = postalCodes; }
    // How the romaji table reads `keys` from the start, unit by unit.
    [[nodiscard]] static std::vector<RomajiToken> ParseRomaji(const RomajiTable& table, std::wstring_view keys);
    // What the user chose before puts candidates first; each commit is
    // recorded. nullptr turns learning off.
    void SetLearning(JapaneseLearningStore* learning) noexcept { learning_ = learning; }
    // Words that start with what is typed, offered below it from the second
    // kana (dictionary and learning). Off by default.
    void SetPredictionEnabled(bool enabled) noexcept { predictionEnabled_ = enabled; }
    void SetPunctuationStyle(PunctuationStyle style) noexcept { punctuation_ = style; }
    // Digits typed in Japanese: half-width (123) or, by default, full-width
    // (１２３). A period or comma between digits is a decimal point or a
    // thousands comma of the same width (3.14), not 。 or 、.
    void SetHalfWidthDigits(bool half) noexcept { halfWidthDigits_ = half; }
    // Letters and symbols typed in Japanese that stay as they are (! ? ( ) q):
    // half-width, or by default full-width. The Japanese marks (、。「」・ー)
    // are not among them.
    void SetHalfWidthSymbols(bool half) noexcept { halfWidthSymbols_ = half; }
    // Hiragana or Katakana: how new text is shown while typing.
    void SetInputForm(KanaForm form) noexcept;
    [[nodiscard]] KanaForm InputForm() const noexcept { return inputForm_; }

    // Live conversion: the text is converted as it is typed and shown so
    // (phrases more than kOpenPhrases back stay as they are); Space then
    // picks another candidate for the last phrase, typing on after it keeps
    // the choice, Enter commits what is shown and Esc shows the kana. Off,
    // Space converts as in Microsoft IME.
    void SetLiveConversion(bool on);
    [[nodiscard]] bool LiveConversion() const noexcept { return live_; }
    static constexpr std::size_t kOpenPhrases = 4;

    [[nodiscard]] bool IsComposing() const noexcept { return !units_.empty() || !pending_.empty(); }
    // True once converted (Space, Muhenkan, F6-F10), and while live
    // conversion shows the text converted.
    [[nodiscard]] bool IsConverted() const noexcept { return !phrases_.empty(); }
    // Live conversion is showing the text converted as typed, with no
    // phrase picked.
    [[nodiscard]] bool IsLivePreview() const noexcept { return preview_ && !phrases_.empty(); }
    // Typing, rather than picking candidates: before conversion, or while
    // the live conversion is shown.
    [[nodiscard]] bool IsTyping() const noexcept { return IsComposing() && (!IsConverted() || preview_); }
    // Whether typing a key commits the conversion first (Microsoft IME);
    // live conversion types on instead.
    [[nodiscard]] bool CommitsBeforeTyping() const noexcept { return IsConverted() && !live_; }

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
    // What was committed last (its last word's part of speech) is where the
    // next conversion starts from, so it reads as the sentence going on.
    // The text service forgets it when the caret is no longer right after
    // that text.
    // The content words of what was committed lately (runs of kanji or
    // katakana) go on as the sentence's other words for the language model.
    void ForgetContext() noexcept {
        context_ = 0;
        contextWords_.clear();
    }
    [[nodiscard]] std::uint16_t Context() const noexcept { return context_; }
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
    // The kana typed so far as the converter reads them, without the keys
    // still pending.
    [[nodiscard]] std::wstring TypedReading() const { return Reading(); }
    [[nodiscard]] std::vector<PreeditSegment> Segments() const;
    // The focused phrase's candidates, and whether the list is open (from
    // the second Space, or an arrow key).
    [[nodiscard]] const std::vector<PhraseCandidate>* FocusedCandidates() const noexcept;
    [[nodiscard]] std::size_t FocusedSelection() const noexcept;
    // The reading the focused phrase was converted from.
    [[nodiscard]] std::wstring FocusedReading() const;
    // The keys each phrase was read from (empty for a phrase that starts or
    // ends inside a romaji unit).
    [[nodiscard]] std::vector<std::wstring> PhraseKeys() const;
    [[nodiscard]] bool IsCandidateListOpen() const noexcept { return listOpen_ && IsConverted(); }

    // Predictions for what is typed (empty once converted), and which one
    // is chosen (Tab, Down and Up step through them; nothing is chosen until
    // then).
    [[nodiscard]] const std::vector<Prediction>& Predictions() const noexcept { return predictions_; }
    [[nodiscard]] std::optional<std::size_t> ChosenPrediction() const noexcept { return chosenPrediction_; }
    void NextPrediction();
    void PreviousPrediction();
    void ChoosePrediction(std::size_t index);
    void ClearPredictionChoice() noexcept { chosenPrediction_.reset(); }
    // Ctrl+Delete: the chosen prediction, or the candidate chosen in the
    // open list, is no longer offered from learning (a prediction leaves
    // the list). Only what was learned can be forgotten.
    [[nodiscard]] bool CanForgetChosen() const;
    bool ForgetChosen();

    // The text to commit (pending keys are resolved first, and a chosen
    // prediction replaces what is typed); clears the composition.
    [[nodiscard]] std::wstring Commit();
    void Clear() noexcept;
    // Ctrl+Backspace right after committing: the text committed last comes
    // back as it was before Enter (the caller takes it out of the
    // document). Typing forgets it.
    [[nodiscard]] const std::wstring* LastCommit() const noexcept {
        return lastCommit_ ? &lastCommit_->text : nullptr;
    }
    bool UndoCommit();
    void ForgetCommit() noexcept { lastCommit_.reset(); }

private:
    struct Unit {
        std::wstring keys;  // what was typed
        std::wstring kana;  // what the table wrote for it
        bool ascii{false};  // typed after a capital: letters as typed, half-width
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
        // The list has been given what the keys mean with a slip undone.
        bool slipsAdded{false};
    };

    void Feed(wchar_t key);
    // Letters after a capital stay letters; whether `key` was taken so.
    bool FeedAscii(wchar_t key);
    [[nodiscard]] bool HasAscii() const noexcept;
    // reading from `from` on, converted (letters after a capital as typed).
    [[nodiscard]] std::vector<Phrase> ConvertReading(const std::wstring& reading, std::size_t from,
                                                     std::uint16_t context,
                                                     const std::vector<std::wstring>& words) const;
    [[nodiscard]] static std::vector<PhraseCandidate> AsciiCandidates(std::wstring_view text);
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
    // The punctuation style and the digits' width.
    [[nodiscard]] std::wstring ApplyTypingStyle(std::wstring text) const;
    [[nodiscard]] std::wstring RenderTyping(bool includePending) const;
    [[nodiscard]] std::wstring PhraseText(const PhraseState& phrase) const;
    [[nodiscard]] std::wstring FormText(KanaForm form, std::size_t begin, std::size_t length,
                                        int letterCase) const;
    // Phrases from the converter, or one phrase for the whole text.
    void BuildPhrases(bool convert);
    [[nodiscard]] std::vector<PhraseCandidate> KanaCandidates(std::wstring_view reading) const;
    void AddPhrases(std::vector<Phrase> phrases, const std::wstring& reading);
    void UpdatePredictions();
    // The English word after the first candidate that starts in katakana.
    void AddLoanwords(std::vector<PhraseCandidate>& candidates) const;
    // Dates, number forms and symbols for a phrase read as `reading`.
    void AddSpecial(std::wstring_view reading, std::vector<PhraseCandidate>& candidates) const;
    // The sum for arithmetic ending in "=" ("1+2=" -> 3, 1+2=3), or nothing.
    [[nodiscard]] std::vector<PhraseCandidate> SumCandidates(std::wstring_view reading) const;
    // The addresses for a postal code (1000001 -> 東京都千代田区千代田), or nothing.
    [[nodiscard]] std::vector<PhraseCandidate> PostalCandidates(std::wstring_view reading) const;
    // When the list opens: the phrase's keys with one slip undone, as more
    // candidates, so a slip the first choice kept is one pick away; for a
    // short input split into phrases, the whole input too ("hahimemashite":
    // は|秘めまして, and はじめまして).
    void AddSlipCandidates(PhraseState& phrase) const;
    // The readings of `keys` with one slip undone that read as known words
    // at most kSlipMargin less likely than `typedCost`, likeliest first,
    // each with its likeliest text; readings in `skip` are left out.
    [[nodiscard]] std::vector<std::pair<PhraseCandidate, std::wstring>> SlipReadings(
        const std::wstring& keys, std::set<std::wstring> skip, std::int64_t typedCost) const;
    // Whether `corrected` splits into phrases where `reading` did (phrases
    // starting at `starts`), apart from where the two differ.
    [[nodiscard]] bool SamePhrasing(const std::wstring& reading, const std::vector<std::size_t>& starts,
                                    const std::wstring& corrected) const;
    // The focused phrase's chosen candidate when it is the whole input read
    // again: it is then shown, and committed, in place of every phrase.
    [[nodiscard]] const PhraseCandidate* WholeChoice() const noexcept;
    // The context after committing `text` whose last word is `rightId`.
    [[nodiscard]] static std::uint16_t ContextAfter(std::wstring_view text, std::uint16_t rightId) noexcept;
    // Keeps the content words of committed `text` for the next conversions.
    void RememberWords(std::wstring_view text);
    // Adds the content words of `text` (runs of kanji or katakana) to `words`.
    static void AddContentWords(std::wstring_view text, std::vector<std::wstring>& words);

    // Live conversion: the typed kana converted again, the settled phrases
    // kept as they are, and all but the last kOpenPhrases settled.
    void UpdateLive();
    // Back to typing after picking: every phrase stays as picked.
    void SettleAll();
    // From the live conversion to picking, on the last phrase.
    void StartPicking();
    void ResetLive() noexcept;

    // What Commit cleared, for UndoCommit.
    struct CommitRecord {
        std::wstring text;
        std::vector<Unit> units;
        std::vector<PhraseState> phrases;
        std::wstring conversionReading;
        std::vector<std::size_t> readingKeys;
        std::uint16_t context{0};
        std::vector<std::wstring> contextWords;
        std::size_t focus{0};
        bool preview{false};
        bool asciiMode{false};
        int capitals{0};
        std::size_t settledPhrases{0};
        std::size_t settledLength{0};
    };

    const RomajiTable* table_{nullptr};
    const JapaneseConverter* converter_{nullptr};
    const KeyConverter* keyConverter_{nullptr};
    const Loanwords* loanwords_{nullptr};
    const JapaneseUserDictionary* userDictionary_{nullptr};
    const SpecialConversions* special_{nullptr};
    const PostalCodes* postalCodes_{nullptr};
    SpecialConversionOptions specialOptions_;
    JapaneseLearningStore* learning_{nullptr};
    std::vector<Unit> units_;
    // Units before the caret; pending keys belong at the caret.
    std::size_t caret_{0};
    std::wstring pending_;
    KanaForm inputForm_{KanaForm::Hiragana};
    PunctuationStyle punctuation_{PunctuationStyle::ToutenKuten};
    bool halfWidthDigits_{false};
    bool halfWidthSymbols_{false};
    // A capital started letters as typed (Google Japanese Input's "Shift
    // for alphanumeric"); the capitals typed in a row.
    bool asciiMode_{false};
    int capitals_{0};
    std::vector<PhraseState> phrases_;
    // The reading the phrases were converted from: the typed one, or the
    // corrected one after a romaji correction (Esc still goes back to what
    // was typed).
    std::wstring conversionReading_;
    // For each position in conversionReading_ (and its end), the key it
    // was read from, or npos inside a word or unit.
    std::vector<std::size_t> readingKeys_;
    std::uint16_t context_{0};
    std::vector<std::wstring> contextWords_;
    std::size_t focus_{0};
    bool listOpen_{false};
    bool live_{false};
    // phrases_ is the live conversion shown while typing.
    bool preview_{false};
    // Esc showed the kana: typing brings the live conversion back.
    bool kanaView_{false};
    // The first phrases, and the reading they cover, that stay as they are.
    std::size_t settledPhrases_{0};
    std::size_t settledLength_{0};
    bool predictionEnabled_{false};
    std::vector<Prediction> predictions_;
    std::optional<std::size_t> chosenPrediction_;
    std::optional<CommitRecord> lastCommit_;
};

}  // namespace tekito::japanese
