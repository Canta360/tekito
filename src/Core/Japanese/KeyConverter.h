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
    };

    KeyConverter(const JapaneseDictionary& dictionary, const ConnectionMatrix& matrix,
                 const JapaneseConverter& converter, const RomajiTable& table, Costs costs = {}) noexcept
        : dictionary_(dictionary), matrix_(matrix), converter_(converter), table_(table), costs_(costs) {}

    // The conversion when the likeliest reading has a corrected slip in it;
    // nothing when it is the keys as typed (the usual conversion handles
    // that) or when keys stay unreadable.
    // `context` and `contextWords`: as for JapaneseConverter::Convert.
    [[nodiscard]] std::optional<KeyConversion> Convert(std::wstring_view keys, std::uint16_t context = 0,
                                                       std::span<const std::wstring> contextWords = {}) const;

private:
    const JapaneseDictionary& dictionary_;
    const ConnectionMatrix& matrix_;
    const JapaneseConverter& converter_;
    const RomajiTable& table_;
    Costs costs_;
};

}  // namespace tekito::japanese
