#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// How a katakana word sounds, loosely: hiragana, with the long-vowel mark
// as the vowel it lengthens, small vowels full size, and sounds katakana
// spells two ways made one (ティ and チ, ヂ and ジ). The japanese-loanwords
// pack is keyed by it (scripts/en/build-expression-packs.py does the same).
[[nodiscard]] std::wstring LoanwordKey(std::wstring_view kana);

// English words for katakana and katakana for English: the japanese-english
// pack (loanwords from JMdict, names from Japanese Wikipedia: グーグル and
// Google), then the japanese-loanwords pack (English words by how they
// sound in katakana, generated from the japanese-phonetic pack).
class Loanwords final {
public:
    // The japanese-loanwords pack.
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    // The japanese-english pack.
    bool OpenEnglish(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return words_.IsOpen() || names_.IsOpen(); }
    // The English words `katakana` may be, best first: written as in the
    // japanese-english pack (Google, iPhone), then by sound.
    [[nodiscard]] std::vector<std::wstring> Words(std::wstring_view katakana, std::size_t limit) const;

    struct Spelling {
        std::wstring english;
        std::wstring katakana;
        // How well known: log2 of the Wikipedia editions, or 6 for a common
        // loanword (3 otherwise).
        double score{0.0};
    };
    // `english` (any case, spaces left out) as it is written and in
    // katakana, best known first: github -> GitHub, ギットハブ.
    [[nodiscard]] std::vector<Spelling> ForEnglish(std::wstring_view english, std::size_t limit) const;
    // How often `word` (lower case) is written in Latin letters inside
    // Japanese text: log10 per million sentences, or nothing when it is not
    // seen there (take) or the pack has no counts.
    [[nodiscard]] std::optional<double> InJapaneseText(std::wstring_view word) const;
    [[nodiscard]] bool HasJapaneseTextCounts() const noexcept { return latin_.IsOpen(); }

private:
    SortedTsv words_;
    SortedTsv names_;
    SortedTsv english_;
    SortedTsv latin_;
};

}  // namespace tekito::japanese
