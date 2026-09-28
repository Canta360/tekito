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

struct PhraseCandidate {
    enum class Kind : std::uint8_t { Dictionary, Hiragana, Katakana, English };

    std::wstring text;
    // Lower is more likely, in the matrix's cost units.
    std::int64_t cost{0};
    Kind kind{Kind::Dictionary};
    // Uses a word Mozc lists as a spelling correction ("もしかして").
    bool spellingCorrection{false};
    // Read from the keys with a slip undone (offered in the list, "もしかして").
    bool slip{false};
    // Set when the candidate is the whole input read again (a slip across
    // phrases): choosing it makes this the reading, as one phrase.
    std::wstring reading;
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
    [[nodiscard]] std::vector<Phrase> Convert(std::wstring_view reading,
                                              std::span<const std::size_t> fixedLengths = {}) const;

    // Only the likeliest text for the whole reading and its cost, much
    // quicker than Convert; nothing when a character is in no word.
    [[nodiscard]] std::optional<PhraseCandidate> Best(std::wstring_view reading) const;

    // Words whose reading starts with `reading` and is longer, likeliest
    // first. Readings shared by too many words to look through quickly give
    // nothing; typing on narrows them.
    [[nodiscard]] std::vector<Prediction> Predict(std::wstring_view reading, std::size_t limit) const;

    static constexpr std::size_t kMaxCandidates = 40;

private:
    const JapaneseDictionary& dictionary_;
    const ConnectionMatrix& matrix_;
};

}  // namespace tekito::japanese
