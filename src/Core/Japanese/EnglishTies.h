#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// How much two words go together, read through English: the english-pairs
// pack (scripts/en/build-english-pairs-pack.py: English words that stand
// near each other, as pointwise mutual information) with the everyday
// English words a Japanese word is glossed with (japanese-english-senses,
// from JMdict: 直す repair, fix, correct; 治す cure, heal). English has far
// more text than the Japanese data has ties, and English typed among
// Japanese (error を 直す) has none in Japanese at all: fix and error go
// together, cure and error do not.
class EnglishTies final {
public:
    // The two packs' folders; true when both open.
    bool Open(const std::filesystem::path& pairsPack, const std::filesystem::path& sensesPack) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return pairs_.IsOpen() && senses_.IsOpen(); }

    // The English base forms `word` stands for: a word in Latin letters as
    // itself in lower case (errors: error); a Japanese one as its English
    // glosses, read as written or as a dictionary form of its stem (直し:
    // 直す).
    [[nodiscard]] std::vector<std::string> Meanings(std::wstring_view word) const;
    // The strongest tie between any of `first` and any of `second`; 0 for none.
    [[nodiscard]] double Tie(const std::vector<std::string>& first, const std::vector<std::string>& second) const;

private:
    [[nodiscard]] std::vector<std::string> Senses(std::wstring_view word) const;

    SortedTsv pairs_;
    SortedTsv senses_;
};

}  // namespace tekito::japanese
