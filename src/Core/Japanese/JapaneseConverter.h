#pragma once

#include <algorithm>
#include <functional>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class TopicVectors;
class EnglishTies;
class Collocations;

class Compounds;
class Notation;
class EdgeScorer;
class ConnectionMatrix;
class JapaneseDictionary;
class LanguageModel;

struct PhraseCandidate {
    // Special: a date, a number written another way, a symbol or a sum
    // (SpecialConversions), which is not learned.
    enum class Kind : std::uint8_t { Dictionary, Hiragana, Katakana, English, Special };

    std::wstring text;
    // Lower is more likely, in the matrix's cost units.
    std::int64_t cost{0};
    Kind kind{Kind::Dictionary};
    // Uses a word Mozc lists as a spelling correction ("moshikashite").
    bool spellingCorrection{false};
    // Read from the keys with a slip undone (offered in the list, "moshikashite").
    bool slip{false};
    // Set when the candidate is the whole input read again (a slip across
    // phrases): choosing it makes this the reading, as one phrase.
    std::wstring reading;
    // The part of speech (right id) of its last word, for what is typed
    // next; 0 when not known.
    std::uint16_t rightId{0};
    // Has a number read in kana in it, written in digits (にせんご: 2005).
    bool kanaNumber{false};
    // Has a set phrase in it (画竜点睛), which the sentence check leaves.
    bool setPhrase{false};
    // Has a name in it (東武, 西友, コカ・コーラ: JapaneseConverter::SetNameParts),
    // which the sentence check leaves when it comes first.
    bool name{false};
    // Has an organization's name in it (ドン・キホーテ), written with its ・.
    bool organization{false};
    // Mix: the chunk in the other language (Japanese read as English, or
    // English as Japanese), as likely as the first. It keeps the place
    // right after the first: other second choices (a slip undone) go after.
    bool otherLanguage{false};
    // A word longer than the phrase it is offered for, over this many
    // characters of the reading from the phrase's start (へ|いき: 併記 over
    // へいき); choosing it makes the phrase that long. 0: the phrase's own.
    std::size_t length{0};
};

// A word that starts with what has been typed, for prediction.
struct Prediction {
    std::wstring reading;
    std::wstring text;
    std::int64_t cost{0};
};

// How written text reads (for reconversion): the reading, and the words
// it was read as, each as (length in the text, length in the reading).
struct ReadingGuess {
    std::wstring reading;
    std::vector<std::pair<std::size_t, std::size_t>> pieces;
    // The last word's part of speech (its right id), as a conversion after
    // the text starts from.
    std::uint16_t right{0};
};

// One phrase (bunsetsu) of the reading and what it can be written as, most
// likely first.
struct Phrase {
    std::size_t begin{0};
    std::size_t length{0};
    std::vector<PhraseCandidate> candidates;
};

// A way to write a whole reading, from its words as they follow each other
// (not phrase by phrase), with where its phrases would break.
struct SentenceGuess {
    std::wstring text;
    std::int64_t cost{0};
    std::vector<std::size_t> breaks;
};

// Kana-kanji conversion over the japanese-core dictionary: the most likely
// words for the reading (Viterbi over the dictionary lattice with the
// connection costs), split into phrases by Mozc's segmentation rules, with
// the likeliest alternatives for each phrase.
class JapaneseUserDictionary;

class JapaneseConverter final {
public:
    JapaneseConverter(const JapaneseDictionary& dictionary, const ConnectionMatrix& matrix) noexcept
        : dictionary_(dictionary), matrix_(matrix) {}

    // `fixedLengths` are phrase lengths from the start that must stay as
    // they are (after the user resized a phrase); the rest is split freely.
    // `context` is the right id of the word just before the reading (the
    // text committed last), 0 for the start of a sentence; `contextWords`
    // are content words written just before, for the language model's
    // sentence ties.
    // `user` adds the user's words; the phrase read exactly like one of
    // them offers it first.
    // `breaks` are positions in the reading where a phrase must end (where
    // the user marked one while typing); unlike fixedLengths, the phrases
    // between them are still split by the rules.
    [[nodiscard]] std::vector<Phrase> Convert(std::wstring_view reading,
                                              std::span<const std::size_t> fixedLengths = {},
                                              std::uint16_t context = 0,
                                              std::span<const std::wstring> contextWords = {},
                                              const JapaneseUserDictionary* user = nullptr,
                                              std::span<const std::size_t> breaks = {}) const;

    // The `count` likeliest texts for the whole reading, each once, cheapest
    // first: unlike Convert's phrases, they may split the reading in other
    // places (作成 or 作 成).
    [[nodiscard]] std::vector<SentenceGuess> BestSentences(std::wstring_view reading, std::size_t count,
                                                           std::uint16_t context = 0,
                                                           const JapaneseUserDictionary* user = nullptr) const;

    // Only the likeliest text for the whole reading and its cost, much
    // quicker than Convert (and without the language model); nothing when
    // a character is in no word.
    // Whether `reading` is a word of the dictionary as a whole (たけ: 竹;
    // not いっすえ, which only makes 逸 and 陶).
    [[nodiscard]] bool IsWord(std::wstring_view reading) const;
    // The dictionary's words read exactly as `reading`, likeliest first
    // (へいき: 兵器, 平気, 併記); spelling corrections left out.
    [[nodiscard]] std::vector<PhraseCandidate> Words(std::wstring_view reading) const;
    // How strongly the word at the start of `text` (a run of kanji or
    // katakana and the kana after it: 売った) shares sentences with any of
    // `others` (LanguageModel::Topic), read as written and as the forms a
    // dictionary lists it by (売る); 0 without a tie, nothing when the
    // language model knows none of those (or is not there).
    [[nodiscard]] std::optional<double> TopicTie(std::wstring_view text, std::span<const std::wstring> others) const;
    // How long the longest word the language model knows at the start of
    // `text` is (差し支えのない: 4, 差し支え; 指し支えのない: 2, 指し).
    [[nodiscard]] std::size_t KnownLength(std::wstring_view text) const;
    // How well `word` goes on from the end of `before` as one (映画 + 界:
    // 映画界), the language model's pointwise mutual information of the
    // pair, with the longest end of `before` it knows; nothing when it has
    // no such pair.
    [[nodiscard]] std::optional<double> PairFit(std::wstring_view before, std::wstring_view word) const;
    [[nodiscard]] std::optional<PhraseCandidate> Best(std::wstring_view reading, std::uint16_t context = 0,
                                                      const JapaneseUserDictionary* user = nullptr) const;
    // What the likeliest words for the whole reading cost, as Convert finds
    // them (with the language model), after `context`; nothing when there
    // are none.
    [[nodiscard]] std::optional<std::int64_t> Cost(std::wstring_view reading, std::uint16_t context = 0,
                                                   const JapaneseUserDictionary* user = nullptr) const;

    // The words Convert chooses from for the whole reading, for training a
    // model that scores them (scripts/ja/train-japanese-edges.py): each
    // word's span, parts of speech, cost and text; what two words next to
    // each other cost beyond the connection matrix (the language model's
    // pairs, compounds), where it is not 0; and the cheapest path, as
    // indexes into `words`.
    struct LatticeWord {
        std::uint32_t begin{0};
        std::uint32_t end{0};
        std::uint16_t left{0};
        std::uint16_t right{0};
        std::int64_t cost{0};
        bool known{false};
        std::wstring text;
    };
    struct LatticeJoin {
        int left{0};
        int right{0};
        std::int64_t cost{0};
    };
    struct LatticeExport {
        std::vector<LatticeWord> words;
        std::vector<LatticeJoin> joins;
        std::vector<int> best;
        std::int64_t bestCost{0};
    };
    [[nodiscard]] LatticeExport ExportLattice(std::wstring_view reading, std::uint16_t context = 0) const;

    // The likeliest reading of written `text` (kanji and kana), from the
    // words it is made of; nothing when a kanji is in no word. Looks
    // through the whole dictionary, so it is for a key press, not typing.
    [[nodiscard]] std::optional<ReadingGuess> ReadingOf(std::wstring_view text) const;
    // Whether `text` writes `reading` word by word: dictionary words read as
    // their part of the reading, kana as themselves, and other characters
    // (digits, letters, marks) as typed. An AI model's writing is taken
    // only so.
    [[nodiscard]] bool CanWrite(std::wstring_view reading, std::wstring_view text) const;
    // How `text` writes `reading` so: the places, as (in the reading, in the
    // text), where its words and kana meet, from (0, 0) to the ends; nothing
    // when it does not.
    [[nodiscard]] std::optional<std::vector<std::pair<std::size_t, std::size_t>>> Align(
        std::wstring_view reading, std::wstring_view text) const;

    // Words whose reading starts with `reading` and is longer, likeliest
    // first. Readings shared by too many words to look through quickly give
    // nothing; typing on narrows them.
    // The user's words come first.
    [[nodiscard]] std::vector<Prediction> Predict(std::wstring_view reading, std::size_t limit,
                                                  const JapaneseUserDictionary* user = nullptr) const;

    static constexpr std::size_t kMaxCandidates = 40;

    // How much the language model (the japanese-lm pack) counts, in the
    // connection matrix's units; tuned with tekito_ja_eval.
    struct ModelWeights {
        // Neighbors that go together make a reading likelier, and common
        // words that never meet less likely: each pair's pointwise mutual
        // information, in nats, times costPerNat when positive,
        // penaltyPerNat when negative. Pairs only somewhat likelier than
        // chance (under `threshold` nats) are left to the parts of speech,
        // which already expect them.
        std::int64_t costPerNat{250};
        std::int64_t penaltyPerNat{200};
        double threshold{1.0};
        // A phrase's candidates are put in order again by how well their
        // word keeps to the sentence's other words (costPerTopic per unit
        // of LanguageModel::Topic), so "injection" comes before "parking"
        // (both chuusha) after "anesthetic".
        std::int64_t costPerTopic{300};
        // And by how close each one's word is to the topic the other
        // phrases' words and those written just before make (the
        // japanese-topics pack): costPerVector per unit of cosine.
        std::int64_t costPerVector{20000};
        // And by how much its English meanings go with those of the words
        // next to its phrase and the words before (EnglishTies: error を
        // 直す, fix error): costPerEnglish per nat of the strongest pair.
        std::int64_t costPerEnglish{300};
        // The same for the Japanese words of the phrases next to it, read
        // in English too (costPerEnglishJapanese; 0: off).
        std::int64_t costPerEnglishJapanese{0};
        // And by how its word goes with the word on the other side of the
        // particle before or after it (Collocations: エラー を 直 often,
        // エラー を 治 never): costPerCollocation per nat.
        std::int64_t costPerCollocation{150};
    };
    // Digits (and a decimal point or comma between them) read as one number
    // word of this part of speech (japanese-core/pos.tsv, "number"), so
    // counters join them (3 and "ko" is three of a thing). Without it they
    // are unknown characters.
    struct NumberWord {
        std::uint16_t left{0};
        std::uint16_t right{0};
        std::int64_t cost{0};
        // Letters only: which are known abbreviations (USJ, PC), set by
        // SetKnownLetters.
        const std::function<bool(std::wstring_view)>* known{nullptr};
    };
    void SetNumberWord(std::optional<NumberWord> number) noexcept { number_ = number; }
    // Letters read by their names (ぴーしー: PC, でぃーえぬえー: DNA) as a word
    // of these parts of speech (a proper noun), offered beside the katakana.
    void SetLetterWord(std::optional<NumberWord> letters) noexcept {
        letters_ = letters;
        if (letters_) letters_->known = knownLetters_ ? &knownLetters_ : nullptr;
    }
    // Which letters are known abbreviations (USJ, PC): they come before a
    // word of the dictionary read the same, other letters after it
    // (えいえいおー: エイエイオー, not AAO). Without: every letters after it.
    // At the start of what is typed with nothing before it, a word that
    // joins the word before it (a particle, read from the matrix: no phrase
    // starts between `content`, a noun's right id, and it) costs `penalty`
    // more (へいき: 平気, not へ行き). 0: off.
    void SetJoinedStart(std::uint16_t content, std::int64_t penalty) noexcept {
        startContent_ = content;
        startPenalty_ = penalty;
    }
    // Whether what is converted next starts a sentence (the text before the
    // caret ends one, or there is none): only then does SetJoinedStart's
    // cost apply; typed into the middle of a sentence, a particle may well
    // come first. Set before converting, as SetNotationLevel.
    void SetAtSentenceStart(bool start) const noexcept { atSentenceStart_ = start; }
    void SetKnownLetters(std::function<bool(std::wstring_view)> known) {
        knownLetters_ = std::move(known);
        if (letters_) letters_->known = knownLetters_ ? &knownLetters_ : nullptr;
    }
    // The edge scorer's score taken off each word's cost before the paths
    // are searched (Convert, BestSentences), for words on a path within
    // `margin` of the cheapest, times `weight`; nullptr: off.
    // `SetEdgeContext`: the text before the caret it reads, set before
    // converting.
    void SetEdgeScorer(const EdgeScorer* scorer, std::int64_t margin = 5000, float weight = 1.0f) noexcept {
        edgeScorer_ = scorer;
        edgeMargin_ = margin;
        edgeWeight_ = weight;
    }
    void SetEdgeContext(std::wstring_view context) const { edgeContext_ = std::wstring(context); }

    // nullptr turns the model off.
    void SetLanguageModel(const LanguageModel* model, ModelWeights weights) noexcept {
        model_ = model;
        weights_ = weights;
    }
    // The topic vectors (ModelWeights::costPerVector); nullptr: off.
    void SetTopicVectors(const TopicVectors* vectors) noexcept { vectors_ = vectors; }
    // The English ties (ModelWeights::costPerEnglish); nullptr: off.
    void SetEnglishTies(const EnglishTies* ties) noexcept { englishTies_ = ties; }
    // The particle pairs (ModelWeights::costPerCollocation); nullptr: off.
    void SetCollocations(const Collocations* collocations) noexcept { collocations_ = collocations; }
    // A phrase of a word and kana after it (こうそうが) also offers every
    // word read like that word, with the same kana (紅藻が).
    void SetHeadWords(bool on) noexcept { headWords_ = on; }
    // How readily kanji join into compounds (japanese-compounds): a word
    // and a kanji that often ends compounds (燃焼 筒), or a kanji that often
    // starts them and a word (再 利用), cost less by costPerNat times ln(1 +
    // the compounds counted); nullptr: off.
    void SetCompounds(const Compounds* compounds, std::int64_t costPerNat) noexcept {
        compounds_ = compounds;
        compoundCost_ = costPerNat;
    }
    // How words are written, kanji or kana (japanese-notation), at a level
    // from 1 (more kanji) to 5 (more kana): a word in the form the level
    // does not want costs `cost` more; nullptr: off. `SetNotationLevel`:
    // the user's level, set before converting.
    void SetNotation(const Notation* notation, std::int64_t cost = 3000) noexcept {
        notation_ = notation;
        notationCost_ = cost;
    }
    void SetNotationLevel(int level) const noexcept { notationLevel_ = level; }
    // The parts of speech (left ids) of names the sentence check leaves
    // (PhraseCandidate::name): organizations (東武, 西友) and the like.
    // `organization`: the part of organizations (0: none), written with
    // their ・ (PhraseCandidate::organization).
    void SetNameParts(std::vector<std::uint16_t> parts, std::uint16_t organization = 0) {
        nameParts_ = std::move(parts);
        organizationPart_ = organization;
    }
    [[nodiscard]] bool IsNamePart(std::uint16_t part) const noexcept {
        return std::find(nameParts_.begin(), nameParts_.end(), part) != nameParts_.end();
    }
    // With the default weights (not "= {}", which GCC and Clang reject for
    // a nested struct with member initializers).
    void SetLanguageModel(const LanguageModel* model) noexcept { SetLanguageModel(model, ModelWeights{}); }

private:
    const JapaneseDictionary& dictionary_;
    const ConnectionMatrix& matrix_;
    const LanguageModel* model_{nullptr};
    const TopicVectors* vectors_{nullptr};
    const EnglishTies* englishTies_{nullptr};
    const Collocations* collocations_{nullptr};
    bool headWords_{true};
    const Compounds* compounds_{nullptr};
    std::int64_t compoundCost_{0};
    ModelWeights weights_;
    std::optional<NumberWord> number_;
    std::optional<NumberWord> letters_;
    const EdgeScorer* edgeScorer_{nullptr};
    std::int64_t edgeMargin_{5000};
    float edgeWeight_{1.0f};
    mutable std::wstring edgeContext_;
    const Notation* notation_{nullptr};
    std::int64_t notationCost_{3000};
    mutable int notationLevel_{3};
    std::vector<std::uint16_t> nameParts_;
    std::function<bool(std::wstring_view)> knownLetters_;
    std::uint16_t startContent_{0};
    std::int64_t startPenalty_{0};
    mutable bool atSentenceStart_{true};
    std::uint16_t organizationPart_{0};
};

}  // namespace tekito::japanese
