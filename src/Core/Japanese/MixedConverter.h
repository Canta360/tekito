#pragma once

#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/MappedFile.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class ConnectionMatrix;
class JapaneseDictionary;
class RomajiTable;

// The japanese-english-words pack: common English words and how common each
// is (log10 of its frequency), mapped read-only and searched in place.
class EnglishWords final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return !lines_.empty(); }
    // The word's score, if it is one of the pack's words (lower case).
    [[nodiscard]] std::optional<double> Score(std::string_view lowercaseWord) const noexcept;

private:
    MappedFile file_;
    std::vector<std::uint32_t> lines_;  // offsets of the rows, in word order
};

// What the keys typed become when Japanese and English are mixed, or when
// a slip in the keys was corrected.
struct MixedConversion {
    // The text the phrases index: kana for the Japanese runs (as corrected),
    // the English words as typed.
    std::wstring reading;
    std::vector<Phrase> phrases;
    bool english{false};
    bool corrected{false};
};

// Converts the keys as typed, not the kana they made while typing: Japanese
// words (the keys read as romaji from any position), English words (the
// keys as they are) and Japanese words one slip away from the keys (a
// neighboring key, a key dropped or added, two keys swapped) go into one
// lattice over the key positions, and the likeliest reading of the whole
// wins ("kyouhameetinggaaru" -> 今日は meeting がある; "arigayou" ->
// ありがとう). English words join the Japanese around them as nouns.
class MixedConverter final {
public:
    // In the connection matrix's units. An English word costs
    // englishBase - englishPerScore * score + languageSwitch, so a reading
    // that is good Japanese stays Japanese; a word read through a slip costs
    // `typo` more than the word. Tuned with tekito_ja_eval; typo 0 turns
    // the correction off.
    struct Costs {
        std::int64_t englishBase{10000};
        std::int64_t englishPerScore{1000};
        std::int64_t languageSwitch{2500};
        std::int64_t typo{6000};
    };

    MixedConverter(const JapaneseDictionary& dictionary, const ConnectionMatrix& matrix,
                   const JapaneseConverter& converter, const RomajiTable& table, const EnglishWords& english,
                   Costs costs = {}) noexcept
        : dictionary_(dictionary), matrix_(matrix), converter_(converter), table_(table), english_(english),
          costs_(costs) {}

    // The conversion when the likeliest reading has an English word or a
    // corrected slip in it; nothing when it is the keys as Japanese (the
    // usual conversion handles that) or when keys stay unreadable. The
    // English words may be missing (no English then).
    [[nodiscard]] std::optional<MixedConversion> Convert(std::wstring_view keys) const;

private:
    const JapaneseDictionary& dictionary_;
    const ConnectionMatrix& matrix_;
    const JapaneseConverter& converter_;
    const RomajiTable& table_;
    const EnglishWords& english_;
    Costs costs_;
};

}  // namespace tekito::japanese
