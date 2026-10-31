#pragma once

#include "Core/Japanese/ChunkJudge.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/KanjiSets.h"
#include "Core/SpecialConversions.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tekito {
class EnglishSpelling;
}

namespace tekito::japanese {

class ChunkJudge;
class JapaneseLearningStore;
class KeyConverter;
class JapaneseUserDictionary;
class Loanwords;
class PostalCodes;
class RomajiTable;
class SentenceReranker;
class KanaSlips;

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
    // Mixed typing: chunks before the one being typed, held with it until
    // Enter so the sentence is judged as a whole.
    bool settled{false};
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
    // Candidates use only kanji in `range` (教育, 常用, 人名用 from `sets`).
    void SetKanjiSets(const KanjiSets* sets) noexcept { kanjiSets_ = sets; }
    void SetKanjiRange(KanjiRange range, int grade = 6, bool names = false) noexcept {
        kanjiRange_ = range;
        kanjiGrade_ = grade;
        kanjiNames_ = names;
    }
    // Mixed typing: Space (and Enter) takes what was typed as one chunk of
    // English or Japanese (ChunkJudge). English stays English, written as
    // it is (GitHub), and is committed with a space after it, as English is
    // typed; Japanese that follows takes that space back (TakeSpaceBefore).
    // The other language comes next in the list. Japanese after English reads on from
    // `englishContext`, the part of speech of a noun. nullptr: off.
    void SetMixedTyping(const ChunkJudge* judge, std::uint16_t englishContext = 0) noexcept {
        chunkJudge_ = judge;
        englishContext_ = englishContext;
        englishRun_ = 0;
    }
    [[nodiscard]] bool MixedTyping() const noexcept { return chunkJudge_ != nullptr; }
    // Mixed typing: English Auto's spelling, so English with a slip in it
    // is put right as in English (rveiew: review). nullptr: as typed.
    void SetEnglishSpelling(const EnglishSpelling* spelling) noexcept { englishSpelling_ = spelling; }
    // Mix: Space puts a slip in an English word right (UserSettings::mixCorrection).
    void SetEnglishCorrection(bool on) noexcept { englishCorrection_ = on; }
    // English words typed into Japanese without spaces (reflectsareta:
    // reflectされた), found with `splitter` in Japanese mode too: first when
    // the keys make no kana, otherwise the next choice. nullptr: off.
    void SetRunTogether(const ChunkJudge* splitter) noexcept { splitter_ = splitter; }
    // Converting checks the likeliest sentences as a whole with `reranker`
    // and takes the one that reads best (not in Mix); nullptr: off.
    void SetSentenceReranker(const SentenceReranker* reranker) noexcept { reranker_ = reranker; }
    // The user's switch for it (on by default).
    void SetSentenceCheck(bool on) noexcept { sentenceCheck_ = on; }
    // The sentences it reads are put together by the converter's costs (on
    // by default) or, off, by the sum of each phrase's rank.
    void SetSentencesByCost(bool on) noexcept { sentencesByCost_ = on; }
    // How much more each candidate of `list` (indices into `candidates`,
    // best first) costs than the first, rising.
    [[nodiscard]] static std::vector<std::int64_t> CostDeltas(const std::vector<PhraseCandidate>& candidates,
                                                              const std::vector<std::size_t>& list);
    // Where in `candidates` the likeliest slip undone goes: right after the
    // first when it is `likelier`, otherwise further down; never before the
    // chunk in the other language, nor second in a chunk taken as English.
    [[nodiscard]] static std::size_t SlipPosition(const std::vector<PhraseCandidate>& candidates, bool likelier) noexcept;
    // Space (and live refine) may undo typing slips in the keys and convert
    // the reading as meant. Off: convert as typed; slip candidates stay in
    // the list. On by default, as UserSettings::japaneseSlipAutoApply.
    void SetSlipAutoApply(bool on) noexcept { slipAutoApply_ = on; }
    [[nodiscard]] bool SlipAutoApply() const noexcept { return slipAutoApply_; }
    // The first emoji for a reading shown on the first page of its list
    // (off: with the other symbols, last).
    void SetEmojiOnFirstPage(bool on) noexcept { emojiOnFirstPage_ = on; }
    // The list ends with what was typed in every form (AddTypedForms).
    void SetTypedForms(bool on) noexcept { typedForms_ = on; }
    // Mixed typing: a vowel typed alone (i, a) with no English just before
    // is kana (い) rather than the letter (I); the other comes next.
    void SetSingleVowelKana(bool on) noexcept { singleVowelKana_ = on; }
    // Live conversion converts again once typing pauses (RefineLive).
    void SetLiveRefine(bool on) noexcept { liveRefine_ = on; }
    // Mixed typing: the chunk being typed is shown converted while it is
    // clearly Japanese (kyouha: 今日は); English, or keys that may be
    // either, show as before.
    void SetMixLiveConversion(bool on) noexcept { mixLive_ = on; }
    // Converting looks for slips in the keys with `slips` (one or two keys
    // dropped, extra, a neighbor or swapped) and converts the keys as
    // meant when they read clearly better (not in Mix); nullptr: off.
    void SetKanaSlips(const KanaSlips* slips) noexcept { slips_ = slips; }
    // What `key` types when it types an opening bracket on its own (「 for
    // [ , in the width set), with the bracket that closes it.
    [[nodiscard]] std::optional<std::pair<std::wstring, std::wstring>> BracketPair(wchar_t key) const;
    // Mixed typing: whether what is typed next starts a sentence, so English
    // there starts with a capital. The text service says so from the text
    // before the caret; committing keeps it up to date.
    void SetSentenceStart(bool start) noexcept {
        sentenceStart_ = start;
        // A new sentence: what came before is not its English or Japanese.
        if (start) {
            japaneseInSentence_ = false;
            englishRun_ = 0;
            englishText_.clear();
        }
    }
    // Whether text ending in `before` has ended its sentence (nothing, a
    // line break, 。 . ! ? and their full-width forms).
    [[nodiscard]] static bool EndsSentence(std::wstring_view before) noexcept;
    // `text` to commit after `before`, with a half-width space between
    // Japanese (kana, kanji) and an English word (letters and digits with a
    // letter: GitHub, iPhone15) on either side, inside it and at its ends
    // against `before`: 今日は GitHub に push した. Not next to digits alone
    // or punctuation; English sentences stay as they are.
    [[nodiscard]] static std::wstring SpacedEnglish(std::wstring_view before, std::wstring_view text);
    // Mixed typing: an English sentence is open, its last word committed
    // with its space (for the period on Enter, as in English Auto).
    [[nodiscard]] bool EnglishSentenceOpen() const noexcept {
        return chunkJudge_ && !IsComposing() && spaceAfterEnglish_ && !japaneseInSentence_;
    }
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
    // A number of one digit is full-width (３), longer ones half-width.
    void SetSingleDigitFullWidth(bool on) noexcept { singleDigitFull_ = on; }
    // How a number typed alone in a phrase converts first.
    enum class NumberStyle {
        AsTyped,     // in the width digits are typed in
        HalfWidth,   // 15
        FullWidth,   // １５
        Mixed,       // one digit full-width (３), more half-width (15)
        Kanji,       // 十五
        Official,    // 1億2,345万6,789, as official documents write numbers
    };
    void SetNumberStyle(NumberStyle style) noexcept { numberStyle_ = style; }
    // Rough numbers in kanji whatever the style: 数十人, 十数人, 二、三日.
    void SetRoughNumbersInKanji(bool on) noexcept { roughKanji_ = on; }
    // Symbols typed in Japanese that stay as they are (! ? ( )): half-width,
    // or by default full-width. The Japanese marks (、。「」・ー) are not
    // among them.
    void SetHalfWidthSymbols(bool half) noexcept { halfWidthSymbols_ = half; }
    // Letters typed in Japanese that stay letters (k, q): half-width, or by
    // default full-width (Settings makes them half-width). Converting offers
    // the other width next.
    void SetHalfWidthLetters(bool half) noexcept { halfWidthLetters_ = half; }
    // Hiragana or Katakana: how new text is shown while typing.
    void SetInputForm(KanaForm form) noexcept;
    [[nodiscard]] KanaForm InputForm() const noexcept { return inputForm_; }
    // The kana a field asks for (a katakana field), over InputForm while
    // typing there; nothing follows InputForm.
    void SetFieldForm(std::optional<KanaForm> form) noexcept { fieldForm_ = form; }

    // Live conversion: the text is converted as it is typed and shown so
    // (phrases more than kOpenPhrases back stay as they are); Space then
    // picks another candidate for the last phrase, typing on after it keeps
    // the choice, Enter commits what is shown and Esc shows the kana. Off,
    // Space converts as in Microsoft IME.
    void SetLiveConversion(bool on);
    [[nodiscard]] bool LiveConversion() const noexcept { return live_; }
    // Live conversion as it applies: not in Mix, where Space judges and
    // converts each chunk.
    [[nodiscard]] bool Live() const noexcept { return live_ && !chunkJudge_; }
    static constexpr std::size_t kOpenPhrases = 4;
    // How many phrases live conversion keeps open (for measuring).
    void SetLiveOpenPhrases(std::size_t count) noexcept { liveOpenPhrases_ = count; }
    // Live conversion, once typing pauses: the whole text converted as Enter
    // converts it (slips undone, checked as a sentence), shown until the
    // next key; Enter then takes it as shown. Whether what is shown changed.
    bool RefineLive();
    // Whether a pause in typing converts the text again (RefineLive).
    [[nodiscard]] bool RefinesOnPause() const noexcept { return liveRefine_ && Live() && IsComposing(); }

    // Text is held: the chunk being typed, or (Mix) the sentence's chunks
    // before it.
    [[nodiscard]] bool IsComposing() const noexcept { return Typing() || !settled_.empty(); }
    // True once converted (Space, Muhenkan, F6-F10), and while live
    // conversion shows the text converted.
    [[nodiscard]] bool IsConverted() const noexcept { return !phrases_.empty(); }
    // Live conversion is showing the text converted as typed, with no
    // phrase picked.
    [[nodiscard]] bool IsLivePreview() const noexcept { return preview_ && !phrases_.empty(); }
    // Typing, rather than picking candidates: before conversion, or while
    // the live conversion is shown.
    [[nodiscard]] bool IsTyping() const noexcept { return Typing() && (!IsConverted() || preview_); }
    // A prediction was picked (Tab) for the text being typed.
    [[nodiscard]] bool PredictionChosen() const noexcept {
        return chosenPrediction_ && *chosenPrediction_ < predictions_.size() && IsTyping();
    }
    // Whether typing a key commits the conversion first (Microsoft IME);
    // live conversion types on instead, and Mix holds the chunk until Enter.
    [[nodiscard]] bool CommitsBeforeTyping() const noexcept { return IsConverted() && !Live() && !chunkJudge_; }

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
        englishRun_ = 0;
        englishText_.clear();
        spaceAfterEnglish_ = false;
        dropSpaceBefore_ = false;
        spaceOwed_ = false;
    }
    // Mixed typing: whether the text just committed follows English whose
    // space it takes back (Helloは, not Hello は). The caller removes the
    // space before it; asking clears it.
    [[nodiscard]] bool TakeSpaceBefore() noexcept {
        const bool drop = dropSpaceBefore_;
        dropSpaceBefore_ = false;
        return drop;
    }
    [[nodiscard]] std::uint16_t Context() const noexcept { return context_; }
    // Space and Henkan: converts, then steps through the candidates.
    void Convert();
    void NextCandidate();
    // Converted, if it was not, with the focused phrase's list open (Tab).
    void ShowCandidates();
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
    // Reconversion: written `text` back in the composition, converted, with
    // the text itself chosen (false when its reading is not known).
    bool Reconvert(std::wstring_view text);
    void ForgetCommit() noexcept { lastCommit_.reset(); }
    // The reading of the text being written (Mix: every chunk, spaced;
    // English as typed), for a report of what it became.
    [[nodiscard]] std::wstring ReportReading() const;

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
        // Learning put another candidate first: the sentence check keeps it.
        bool learned{false};
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
    // A consonant left on its own right before the caret, taken back into
    // the pending keys when `key` goes on from it.
    void RejoinConsonant(wchar_t key);
    // かな入力: a sound mark (゛ ゜) joined to the kana before it; whether it was.
    bool JoinSoundMark(wchar_t key);
    // Some kana were typed as kana (かな入力), not as romaji keys: the romaji
    // slip corrections do not apply.
    [[nodiscard]] bool KanaTyped() const;
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
    // BuildPhrasesFor, then KeepKanjiInRange.
    void BuildPhrases(bool convert);
    void BuildPhrasesFor(bool convert);
    // Candidates with kanji outside SetKanjiRange taken out (the phrase's
    // kana when none is left).
    void KeepKanjiInRange(PhraseState& phrase) const;
    // Mix on Enter: runs of Japanese chunks converted again as one reading,
    // each chunk still its own phrases, so a word reads on from the ones
    // after it (sake wo nomu: 酒を飲む, not 避けを飲む).
    void ConvertJapaneseRuns();
    // The likeliest sentences the phrases' candidates make, checked as a
    // whole (SetSentenceReranker); the one taken is selected.
    void CheckSentence();
    // Each phrase's list ends with what was typed in every form: hiragana,
    // katakana, half-width katakana and the keys in full and half width
    // (what F6-F10 give), for people who look for them there.
    void AddTypedForms();
    // A sentence in parts, each with its choices (best first): the choice
    // of each part the sentence model reads best, all 0 to keep the first.
    // The phrases of a conversion, the chunks of a mixed sentence and the
    // pieces of English run into Japanese are checked with it alike.
    // `deltas` (CostDeltas for each part) puts the sentences it reads in
    // order by the converter's costs; without, by the sum of the ranks.
    [[nodiscard]] std::vector<std::size_t> ReadBest(const std::vector<std::vector<std::wstring>>& parts,
                                                    const std::vector<std::vector<std::int64_t>>* deltas = nullptr) const;
    // A katakana word typed with a vowel for ー (にゅうす: ニュース), into the
    // phrase's list.
    void AddLongVowelWords(std::vector<PhraseCandidate>& candidates, std::wstring_view reading,
                           bool overSlip = false) const;
    // The keys with their likeliest slips undone (SetKanaSlips), converted
    // in place of the phrases when that reads better; whether it did.
    bool FixSlips();
    // Mixed typing: the candidates for a chunk judged English, `written`
    // first, then the keys as typed, katakana and the Japanese.
    [[nodiscard]] std::vector<PhraseCandidate> EnglishCandidates(const std::wstring& written,
                                                                 const std::wstring& reading) const;
    // `written`, with a capital when it starts a sentence (hello: Hello;
    // GitHub stays).
    [[nodiscard]] std::wstring EnglishAsWritten(std::wstring written) const;
    // Punctuation keys typed after an English word (you.), as English
    // writes them in an English sentence and as Japanese does otherwise.
    [[nodiscard]] std::wstring MarksAfterEnglish(std::wstring_view keys) const;
    // Mixed typing: Japanese keys run into an English word with no Space
    // (kyouhagoogle, korehagithub): the Japanese converted and the word,
    // when the keys leave letters romaji cannot read.
    // `foreign`: an English word in it has letters romaji hardly uses.
    [[nodiscard]] std::optional<std::wstring> RunTogetherEnglish(bool* foreign = nullptr) const;
    // Keys that leave letters romaji cannot read and spell an English word
    // (mix, which may come out みん): the word and its katakana, offered
    // right after the first candidate for the whole input.
    void AddTypedEnglish();
    // Mixed typing: whether what is chosen is English (to space it and
    // read on from it).
    [[nodiscard]] bool ChoseEnglish() const noexcept;
    [[nodiscard]] std::vector<PhraseCandidate> KanaCandidates(std::wstring_view reading) const;
    // Letters in the first candidate in the other width, right after it
    // (k: ｋ, ｋ: k).
    void AddOtherWidth(std::vector<PhraseCandidate>& candidates) const;
    // Pending keys as shown: letters and symbols in their widths.
    [[nodiscard]] std::wstring PendingText() const;
    void AddPhrases(std::vector<Phrase> phrases, const std::wstring& reading);
    void UpdatePredictions();
    // The English word after the first candidate that starts in katakana.
    // `reading`: the phrase's kana, looked up too when the dictionary did
    // not know the word (ぐうぐるで: Googleで).
    void AddLoanwords(std::vector<PhraseCandidate>& candidates, std::wstring_view reading) const;
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
    void AddSlipCandidatesFor(PhraseState& phrase) const;
    // The readings of `keys` with one slip undone that read as known words
    // at most kSlipMargin less likely than `typedCost`, likeliest first,
    // each with its likeliest text; readings in `skip` are left out.
    [[nodiscard]] std::vector<std::pair<PhraseCandidate, std::wstring>> SlipReadings(
        const std::wstring& keys, std::set<std::wstring> skip, std::int64_t typedCost) const;
    // Whether `corrected` splits into phrases where `reading` did (phrases
    // starting at `starts`), apart from where the two differ.
    [[nodiscard]] bool SamePhrasing(const std::wstring& reading, const std::vector<std::size_t>& starts,
                                    const std::wstring& corrected) const;
    // The chosen candidate (of the focused phrase, else any) when it is the whole input read
    // again: it is then shown, and committed, in place of every phrase.
    [[nodiscard]] const PhraseCandidate* WholeChoice() const noexcept;
    // A whole-input reading chosen in any phrase goes back to that phrase's
    // own first candidate, so the phrases are shown one by one again.
    void DropWholeChoice() noexcept;
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
    // Live: the converted text left for the typed kana (the caret moves in
    // it), and the conversion shown again once the caret is at the end.
    void LeaveLivePreview();
    void ShowLiveAtEnd();
    // What is shown is RefineLive's for the keys typed.
    [[nodiscard]] bool LiveRefined() const;
    // Live: the whole text converted again as Space converts it before live
    // conversion (slips undone, English run in, the whole input read at
    // once), unless something was picked while typing. True when it ran.
    bool RebuildLive();

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
    const ChunkJudge* chunkJudge_{nullptr};
    const EnglishSpelling* englishSpelling_{nullptr};
    bool englishCorrection_{true};
    const ChunkJudge* splitter_{nullptr};
    // Mixed typing: the sentence's chunks before the one being typed, each
    // as English and as Japanese, shown as the whole sentence judges it.
    struct SettledChunk {
        std::wstring typed;     // the keys as typed (capitals, punctuation)
        std::wstring keys;      // lower case, without the punctuation after
        std::wstring english;   // as English
        std::wstring japanese;  // as Japanese
        // Its other conversions, for the sentence check (japanese first).
        std::vector<std::wstring> japaneseChoices;
        bool isEnglish{false};
        bool marksOnly{false};  // punctuation of its own (a "." after English)
        bool locked{false};     // picked from the list or typed with capitals
        bool picked{false};     // picked from the list, or Japanese taken as typed
        bool sentenceStart{false};  // it began a sentence (English capitalized)
        bool either{false};     // it reads as either language (data, sake)
        std::wstring reading;   // Japanese: the kana converted
        std::uint16_t context{0};  // what conversion went on from
    };
    std::vector<SettledChunk> settled_;
    // English was committed just before the sentence, with its space after.
    bool spaceBeforeSentence_{false};
    // Mixed typing: English was committed last; the next English committed
    // takes a space before it (none is left after text on Enter).
    bool spaceOwed_{false};
    // The chunk CommitChunk took last was English.
    bool lastChunkEnglish_{false};
    // The chunk being typed (not the settled ones).
    [[nodiscard]] bool Typing() const noexcept { return !units_.empty() || !pending_.empty(); }
    // Mixed typing: the chunk being typed shown as its letters rather than
    // kana, when the sentence so far is English or the keys make no kana
    // (yourself: not ようrせlf); nothing otherwise.
    [[nodiscard]] std::optional<std::wstring> TypedLetters() const;
    // Mix live conversion: the chunk being typed, converted, while it is
    // clearly Japanese (SetMixLiveConversion).
    [[nodiscard]] std::optional<std::wstring> MixLiveText() const;
    // Mixed typing: the chunk being typed joins the settled ones.
    void SettleChunk();
    // Mixed typing: the settled chunks as one text, and the whole sentence
    // committed.
    [[nodiscard]] std::wstring SettledText() const;
    [[nodiscard]] static std::wstring JoinChunks(const std::vector<SettledChunk>& chunks);
    // Mixed typing: each settled chunk as English or as one of its
    // conversions, as the sentence model reads the whole sentence best.
    void CheckMixedSentence();
    [[nodiscard]] std::wstring CommitSentence();
    // The chunk being typed, committed (what Commit was before Mix held
    // sentences).
    [[nodiscard]] std::wstring CommitChunk();
    // Mixed typing: the sentence's English and Japanese so far, from the
    // settled chunks.
    void RecountSentence();
    // Mixed typing: the settled chunks and `current` (the chunk being
    // typed, judged) judged again as a whole sentence.
    void JudgeWholeSentence(std::optional<ChunkJudge::Judgement>& current, std::wstring_view currentKeys);
    // Mixed typing: the chunk's keys as typed, capitals kept (they go into
    // the composer in lower case); empty once the caret moved inside it.
    std::wstring typedCase_;
    bool typedCaseKept_{true};
    // Mixed typing: the chunk as typed when a capital was typed in it (I,
    // GitHub): it is English, as typed.
    [[nodiscard]] std::optional<std::wstring> TypedWithCapitals(std::wstring_view keys) const;
    // The chunk judge's judgement of `keys`, with SetSingleVowelKana.
    [[nodiscard]] ChunkJudge::Judgement JudgeChunk(std::wstring_view keys) const;
    // Phrases that are only digits: the form SetNumberStyle asks for first.
    void ApplyNumberStyle();
    // A run of 。 typed as ... is one phrase, … first.
    void JoinEllipsis();
    // Numbers read in kana in the number style, the other way next.
    void StyleKanaNumbers(std::vector<PhraseCandidate>& candidates) const;
    [[nodiscard]] static std::wstring KanaNumberText(const std::wstring& text, NumberStyle style);
    [[nodiscard]] bool SingleVowelKana(std::wstring_view keys) const noexcept;
    std::uint16_t englishContext_{0};
    // Mixed typing: English chunks committed in a row, just before, and
    // their text (context for English Auto's spelling).
    std::size_t englishRun_{0};
    std::wstring englishText_;
    bool sentenceStart_{true};
    // English was committed last, with its space after it.
    bool spaceAfterEnglish_{false};
    // Japanese was committed in this sentence: punctuation after English
    // is Japanese (、。) then, English (, .) in an English sentence.
    bool japaneseInSentence_{false};
    bool dropSpaceBefore_{false};
    const JapaneseUserDictionary* userDictionary_{nullptr};
    const SpecialConversions* special_{nullptr};
    const PostalCodes* postalCodes_{nullptr};
    SpecialConversionOptions specialOptions_;
    JapaneseLearningStore* learning_{nullptr};
    const SentenceReranker* reranker_{nullptr};
    bool sentenceCheck_{true};
    bool sentencesByCost_{true};
    bool slipAutoApply_{true};
    bool emojiOnFirstPage_{true};
    bool typedForms_{true};
    bool singleVowelKana_{true};
    bool liveRefine_{true};
    bool mixLive_{false};
    // MixLiveText's last conversion, by the reading it was for.
    mutable std::wstring mixLiveReading_;
    mutable std::wstring mixLiveText_;
    const KanaSlips* slips_{nullptr};
    std::vector<Unit> units_;
    // Units before the caret; pending keys belong at the caret.
    std::size_t caret_{0};
    std::wstring pending_;
    KanaForm inputForm_{KanaForm::Hiragana};
    std::optional<KanaForm> fieldForm_;
    [[nodiscard]] KanaForm TypingForm() const noexcept { return fieldForm_.value_or(inputForm_); }
    PunctuationStyle punctuation_{PunctuationStyle::ToutenKuten};
    bool halfWidthDigits_{false};
    bool singleDigitFull_{false};
    const KanjiSets* kanjiSets_{nullptr};
    KanjiRange kanjiRange_{KanjiRange::All};
    int kanjiGrade_{6};
    bool kanjiNames_{false};
    NumberStyle numberStyle_{NumberStyle::AsTyped};
    bool roughKanji_{true};
    bool halfWidthSymbols_{false};
    bool halfWidthLetters_{false};
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
    std::size_t liveOpenPhrases_{kOpenPhrases};
    // phrases_ is the live conversion shown while typing.
    bool preview_{false};
    // Esc showed the kana: typing brings the live conversion back.
    bool kanaView_{false};
    // The first phrases, and the reading they cover, that stay as they are.
    std::size_t settledPhrases_{0};
    std::size_t settledLength_{0};
    // The keys RefineLive converted last.
    std::wstring refinedKeys_;
    bool predictionEnabled_{false};
    std::vector<Prediction> predictions_;
    std::optional<std::size_t> chosenPrediction_;
    std::optional<CommitRecord> lastCommit_;
    // The keys were made up from a reading (Reconvert): no slips in them.
    bool reconverted_{false};
};

}  // namespace tekito::japanese
