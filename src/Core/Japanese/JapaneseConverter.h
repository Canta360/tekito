#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class ConnectionMatrix;
class JapaneseDictionary;
class LanguageModel;

struct PhraseCandidate {
    enum class Kind : std::uint8_t { Dictionary, Hiragana, Katakana, English };

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
};

// A word that starts with what has been typed, for prediction.
struct Prediction {
    std::wstring reading;
    std::wstring text;
    std::int64_t cost{0};
};

// One phrase (bunsetsu) of the reading and what it can be written as, most
// likely first.
struct Phrase {
    std::size_t begin{0};
    std::size_t length{0};
    std::vector<PhraseCandidate> candidates;
};

// Kana-kanji conversion over the japanese-core dictionary: the most likely
// words for the reading (Viterbi over the dictionary lattice with the
// connection costs), split into phrases by Mozc's segmentation rules, with
// the likeliest alternatives for each phrase.
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
    [[nodiscard]] std::vector<Phrase> Convert(std::wstring_view reading,
                                              std::span<const std::size_t> fixedLengths = {},
                                              std::uint16_t context = 0,
                                              std::span<const std::wstring> contextWords = {}) const;

    // Only the likeliest text for the whole reading and its cost, much
    // quicker than Convert (and without the language model); nothing when
    // a character is in no word.
    [[nodiscard]] std::optional<PhraseCandidate> Best(std::wstring_view reading, std::uint16_t context = 0) const;

    // Words whose reading starts with `reading` and is longer, likeliest
    // first. Readings shared by too many words to look through quickly give
    // nothing; typing on narrows them.
    [[nodiscard]] std::vector<Prediction> Predict(std::wstring_view reading, std::size_t limit) const;

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
        std::int64_t costPerNat{200};
        std::int64_t penaltyPerNat{200};
        double threshold{1.0};
        // A phrase's candidates are put in order again by how well their
        // word keeps to the sentence's other words (costPerTopic per unit
        // of LanguageModel::Topic), so "injection" comes before "parking"
        // (both chuusha) after "anesthetic".
        std::int64_t costPerTopic{300};
    };
    // nullptr turns the model off.
    void SetLanguageModel(const LanguageModel* model, ModelWeights weights = {}) noexcept {
        model_ = model;
        weights_ = weights;
    }

private:
    const JapaneseDictionary& dictionary_;
    const ConnectionMatrix& matrix_;
    const LanguageModel* model_{nullptr};
    ModelWeights weights_;
};

}  // namespace tekito::japanese
