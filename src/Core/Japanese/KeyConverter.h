#pragma once

#include "Core/Japanese/JapaneseConverter.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class ConnectionMatrix;
class JapaneseDictionary;
class RomajiTable;

// What the keys typed become when a slip in them was corrected.
struct KeyConversion {
    // The kana the phrases index, as corrected.
    std::wstring reading;
    std::vector<Phrase> phrases;
    // For each position in `reading` (and its end), the key position it
    // was read from, or npos inside a word.
    std::vector<std::size_t> keyAt;
};

// Converts the keys as typed, not the kana they made while typing: Japanese
// words (the keys read as romaji from any position) and Japanese words one
// slip away from the keys (a neighboring key, a key dropped or added, two
// keys swapped) go into one lattice over the key positions, and the
// likeliest reading of the whole wins ("arigayou" -> ありがとう).
class KeyConverter final {
public:
    // In the connection matrix's units: a word read through a slip costs
    // `typo` more than the word. Tuned with tekito_ja_eval; 0 turns the
    // correction off.
    struct Costs {
        std::int64_t typo{6000};
        // The slip's cost at the first key and at the last, in percent of
        // `typo`, in between in proportion: a slip late in what is typed is
        // likelier, after more keys went by (tekito_ja_eval: 70% at the end
        // corrects more slips and no sentence typed right).
        std::int64_t earlyPercent{100};
        std::int64_t latePercent{70};
        // The first key typed twice (kkouenni: っ公園に), in percent of
        // `typo`, when at least `doubledFirstKeys` keys are typed: a phrase
        // hardly starts with っ, but a short one may be meant so (kka: っか,
        // put into text being edited).
        std::int64_t doubledFirstPercent{20};
        std::size_t doubledFirstKeys{6};
        // Each kind of slip, in percent of the cost above: a neighboring
        // key, a key left out, a key typed twice or extra, two keys swapped
        // (eval/typo-study.py: keys left out or swapped cheaper put more
        // right and change no text typed right; a neighboring key cheaper
        // converts text typed right worse).
        std::int64_t replacedPercent{100};
        std::int64_t droppedPercent{80};
        std::int64_t addedPercent{100};
        std::int64_t swappedPercent{60};
        // The keys as typed and the likeliest reading with a slip undone
        // are both converted with the language model and the text before
        // them, and the slip is undone when that costs, with the slips,
        // `keepTypedBy` less than the keys as typed (誰おん: 誰の).
        // Off: the key lattice alone decides.
        bool contextDecides{true};
        std::int64_t keepTypedBy{0};
        // A consonant left out (nomiai: nomitai), as well as a vowel or n.
        bool consonantsDropped{true};
        // A vowel typed for another, neighbor or not (miro: miru), as well
        // as a neighboring key.
        bool vowelsReplaced{true};
    };

    KeyConverter(const JapaneseDictionary& dictionary, const ConnectionMatrix& matrix,
                 const JapaneseConverter& converter, const RomajiTable& table, Costs costs) noexcept
        : dictionary_(dictionary), matrix_(matrix), converter_(converter), table_(table), costs_(costs) {}
    // With the default costs (not "= {}", which GCC and Clang reject for a
    // nested struct with member initializers).
    KeyConverter(const JapaneseDictionary& dictionary, const ConnectionMatrix& matrix,
                 const JapaneseConverter& converter, const RomajiTable& table) noexcept
        : KeyConverter(dictionary, matrix, converter, table, Costs{}) {}

    // The conversion when the likeliest reading has a corrected slip in it;
    // nothing when it is the keys as typed (the usual conversion handles
    // that) or when keys stay unreadable.
    // `context` and `contextWords`: as for JapaneseConverter::Convert.
    [[nodiscard]] std::optional<KeyConversion> Convert(std::wstring_view keys, std::uint16_t context = 0,
                                                       std::span<const std::wstring> contextWords = {},
                                                       const JapaneseUserDictionary* user = nullptr) const;

private:
    const JapaneseDictionary& dictionary_;
    const ConnectionMatrix& matrix_;
    const JapaneseConverter& converter_;
    const RomajiTable& table_;
    Costs costs_;
};

}  // namespace tekito::japanese
