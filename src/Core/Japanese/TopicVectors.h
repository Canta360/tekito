#pragma once

#include "Core/Japanese/MappedFile.h"
#include "Core/Japanese/SortedTsv.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace tekito::japanese {

// The japanese-topics pack (ja-topics-v1, scripts/ja/build-japanese-topics-pack.py):
// a vector for each common content word (runs of kanji, katakana words),
// learned so that words used together point the same way. The words
// around a phrase add up to the sentence's topic, and among words that
// read alike the one closest to it fits best (戴冠 near 王 and 聖母, 体感
// near 気温). Mapped and read in place.
class TopicVectors final {
public:
    static constexpr std::size_t kDims = 64;
    using Vector = std::array<float, kDims>;

    bool Open(const std::filesystem::path& packDirectory);
    [[nodiscard]] bool IsOpen() const noexcept { return count_ != 0; }

    // The word's unit vector, if the pack has it.
    [[nodiscard]] std::optional<Vector> Of(std::wstring_view word) const;
    // A topic: unit vectors added up (Add), then compared (Closeness).
    struct Topic {
        Vector sum{};
        std::size_t words{0};
    };
    void Add(Topic& topic, std::wstring_view word) const;
    // Cosine of `word` with the topic: -1 to 1; nothing when either is
    // unknown or empty.
    [[nodiscard]] std::optional<double> Closeness(const Topic& topic, std::wstring_view word) const;

private:
    MappedFile file_;
    SortedTsv words_;
    std::uint32_t count_{0};
};

}  // namespace tekito::japanese
