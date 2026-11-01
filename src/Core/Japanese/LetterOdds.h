#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// The japanese-letter-odds pack (scripts/ja/build-japanese-letter-odds.py):
// how much likelier each letter is, after the two before it, in an English
// spelling than in the romaji of a Japanese reading. Its sum over a word
// tells English no list has (cappuccino, gibberellin: well above 0) from
// Japanese (kyouha: below), with words that read both ways near 0 (note,
// data, sake).
class LetterOdds final {
public:
    bool Open(const std::filesystem::path& packDirectory);
    [[nodiscard]] bool IsOpen() const noexcept { return !odds_.empty(); }
    // ln odds of English over romaji for `lower` (small letters a-z); 0
    // when a character is none of them.
    [[nodiscard]] double Score(std::wstring_view lower) const noexcept;

private:
    std::vector<float> odds_;  // by triple: ^ a-z $, 28 each
};

}  // namespace tekito::japanese
