#pragma once

#include "Core/Japanese/MappedFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace tekito::japanese {

// The japanese-lm pack (ja-lm-v3, scripts/ja/build-japanese-lm.py): how often
// words occur in Japanese text, how much likelier pairs of neighbors are
// than their words apart, and which content words share sentences. Mapped
// and read in place.
class LanguageModel final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return pairSlots_ != 0; }

    // FNV-1a over UTF-16 code units, as the builder hashes; Continue goes on
    // from a hash already taken.
    [[nodiscard]] static std::uint64_t Hash(std::wstring_view text) noexcept;
    [[nodiscard]] static std::uint64_t Continue(std::uint64_t hash, std::wstring_view text) noexcept;
    // The hash a pair starts from: the left word and the separator.
    [[nodiscard]] static std::uint64_t PairStart(std::wstring_view left) noexcept;

    // ln(count) of the word in the corpus, if the model has it.
    [[nodiscard]] std::optional<double> LogCount(std::wstring_view word) const noexcept;
    // ln of the number of words in the corpus.
    [[nodiscard]] double LogTokens() const noexcept { return logTokens_; }
    // The pointwise mutual information of `right` after the left word of
    // `pairStart`, in nats, if the pair was seen.
    [[nodiscard]] std::optional<double> PairPmi(std::uint64_t pairStart, std::wstring_view right) const noexcept;
    // How strongly two content words keep to the same sentences:
    // ln(1 + log-likelihood significance), if they do more than by chance.
    [[nodiscard]] std::optional<double> Topic(std::wstring_view first, std::wstring_view second) const;
    // Whether a word is one the sentence table has: two characters or more,
    // with kanji or katakana.
    [[nodiscard]] static bool IsContentWord(std::wstring_view word) noexcept;

private:
    [[nodiscard]] std::uint8_t Find(std::uint64_t hash, std::uint32_t slots, std::size_t offset) const noexcept;

    MappedFile file_;
    std::uint32_t pairSlots_{0};
    std::size_t pairsOffset_{0};
    std::uint32_t wordSlots_{0};
    std::size_t wordsOffset_{0};
    std::uint32_t topicSlots_{0};
    std::size_t topicsOffset_{0};
    double logTokens_{0.0};
};

}  // namespace tekito::japanese
