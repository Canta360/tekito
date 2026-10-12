#pragma once

#include "Core/Japanese/MappedFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito::japanese {

// The japanese-reranker pack (ja-rerank-v1,
// scripts/ja/build-japanese-reranker-pack.py): a small language model over
// word pieces that says how naturally whole sentences read, to choose
// among the converter's likeliest sentences. Short text gives it little to
// go on, so it counts less there, and it replaces the converter's first
// sentence only when it reads clearly better. The weights are mapped and
// read in place.
class SentenceReranker final {
public:
    bool Open(const std::filesystem::path& packDirectory);
    [[nodiscard]] bool IsOpen() const noexcept { return vocab_ != 0; }
    // How many of the converter's sentences it chooses among.
    [[nodiscard]] std::size_t Top() const noexcept { return prior_.size(); }

    // The pieces `text` is read as, as SentencePiece reads it (NFKC, a
    // space before it, the likeliest pieces, unknown characters as their
    // UTF-8 bytes).
    [[nodiscard]] std::vector<std::uint32_t> Pieces(std::wstring_view text) const;
    // ln p of each sentence: its pieces and the end, after the start.
    [[nodiscard]] std::vector<double> LogProbabilities(std::span<const std::wstring> sentences) const;
    // Which of `sentences` (likeliest first, as the converter ranks them)
    // to take: 0 keeps the first. Only the first Top() are looked at.
    [[nodiscard]] std::size_t Choose(std::span<const std::wstring> sentences) const;

private:
    struct Piece {
        std::uint32_t id;
        float score;
    };
    struct State {
        std::vector<float> h, c;
    };

    [[nodiscard]] State Start() const;
    [[nodiscard]] State Step(const State& state, std::uint32_t piece) const;
    // ln p of every piece after `state`'s, as logits less their log-sum.
    void NextLogits(const State& state, std::vector<float>& logits, float& logSum) const;

    MappedFile file_;
    std::uint32_t vocab_{0}, embed_{0}, hidden_{0};
    float scale_{0}, weight_{0}, rampChars_{0}, margin_{0}, unknownScore_{0};
    std::vector<float> prior_;
    const std::int8_t* embedding_{nullptr};
    const float* embeddingScale_{nullptr};
    const std::int8_t* inputWeights_{nullptr};
    const float* inputScale_{nullptr};
    const std::int8_t* hiddenWeights_{nullptr};
    const float* hiddenScale_{nullptr};
    const float* bias_{nullptr};
    const float* projection_{nullptr};
    const float* projectionBias_{nullptr};

    std::unordered_map<std::wstring, Piece> pieces_;
    std::size_t longestPiece_{0};
    std::uint32_t bytePiece_[256]{};
    std::uint32_t separator_{3}, end_{2};
};

}  // namespace tekito::japanese
