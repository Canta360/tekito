#pragma once

#include "Core/Japanese/MappedFile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class RomajiTable;

// The japanese-slips pack (ja-slips-v1,
// scripts/ja/build-japanese-slips-pack.py): what reads as Japanese kana,
// to find slips in typed keys before conversion. Keys are read with one
// or two edits (a key dropped, an extra key, a neighbor or another key,
// two keys swapped); a kana n-gram model ranks thousands of the readings
// that makes and a small kana LSTM scores the best few. Mapped and read
// in place.
class KanaSlips final {
public:
    bool Open(const std::filesystem::path& packDirectory);
    [[nodiscard]] bool IsOpen() const noexcept { return chars_ != 0; }

    struct Reading {
        std::wstring keys;
        // The kana the keys read as (NFKC), as the models saw them.
        std::wstring kana;
        // ln p of the kana by the LSTM, less what the edits cost.
        double score{0};
        // The edits, as spans of the typed keys replaced: [begin, end) -> text.
        struct Edit {
            std::size_t begin, end;
            std::wstring with;
        };
        std::vector<Edit> edits;
    };
    // The keys as typed first, then the likeliest corrected readings, best
    // first (at most `count` of them). Empty when the keys are too long or
    // the pack is missing.
    [[nodiscard]] std::vector<Reading> Find(std::wstring_view keys, const RomajiTable& table, std::size_t count) const;

    // ln p of `kana` by the n-gram model and by the LSTM (for tests).
    [[nodiscard]] double NgramLogProbability(std::wstring_view kana) const;
    [[nodiscard]] std::vector<double> NeuralLogProbabilities(std::span<const std::wstring> kana) const;

private:
    struct Order {
        const std::uint32_t* codes{nullptr};
        const std::uint16_t* logProb{nullptr};
        const std::uint16_t* logBackoff{nullptr};
        std::size_t count{0};
    };
    [[nodiscard]] std::vector<std::uint8_t> Ids(std::wstring_view kana) const;
    // ln P(ids[at] | the ids before it), at >= 1 (ids[0] is <s>).
    [[nodiscard]] double Contribution(const std::uint8_t* ids, std::size_t at) const;
    [[nodiscard]] std::size_t Find(const Order& order, std::uint32_t code) const;

    MappedFile file_;
    std::uint32_t chars_{0}, order_{0}, embed_{0}, hidden_{0};
    std::vector<Order> orders_;
    std::array<std::uint8_t, 0x10000> charId_{};
    const float* embedding_{nullptr};
    const std::int8_t* inputWeights_{nullptr};
    const float* inputScale_{nullptr};
    const std::int8_t* hiddenWeights_{nullptr};
    const float* hiddenScale_{nullptr};
    const float* bias_{nullptr};
    const std::int8_t* output_{nullptr};
    const float* outputScale_{nullptr};
    const float* outputBias_{nullptr};
};

}  // namespace tekito::japanese
