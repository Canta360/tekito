#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// How a katakana word sounds, loosely: hiragana, with the long-vowel mark
// as the vowel it lengthens, small vowels full size, and sounds katakana
// spells two ways made one (ティ and チ, ヂ and ジ). The japanese-loanwords
// pack is keyed by it (scripts/en/build-expression-packs.py does the same).
[[nodiscard]] std::wstring LoanwordKey(std::wstring_view kana);

// The japanese-loanwords pack: English words by how they sound in katakana
// (ミーティング -> meeting), generated from the japanese-phonetic pack.
class Loanwords final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return words_.IsOpen(); }
    // The English words `katakana` may be, most common first.
    [[nodiscard]] std::vector<std::wstring> Words(std::wstring_view katakana, std::size_t limit) const;

private:
    SortedTsv words_;
};

}  // namespace tekito::japanese
