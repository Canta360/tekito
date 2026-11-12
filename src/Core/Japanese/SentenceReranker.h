#pragma once

#include "Core/Japanese/MappedFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
    // Up to how many characters the sentence model counts for less, in
    // proportion (the pack's own setting; 0: never less).
    [[nodiscard]] float RampChars() const noexcept { return rampChars_; }
    void SetRampChars(float chars) noexcept { rampChars_ = chars; }
    // Whether the text before (Choose's context) counts toward the ramp (on
    // by default): a word typed after it is read with it (経費を + せいさん: 精算).
    void SetRampWithContext(bool on) noexcept { rampWithContext_ = on; }
    [[nodiscard]] bool IsOpen() const noexcept { return vocab_ != 0; }
    // How many of the converter's sentences it chooses among.
    [[nodiscard]] std::size_t Top() const noexcept { return prior_.size(); }

    // The pieces `text` is read as, as SentencePiece reads it (NFKC, a
    // space before it, the likeliest pieces, unknown characters as their
    // UTF-8 bytes).
    [[nodiscard]] std::vector<std::uint32_t> Pieces(std::wstring_view text) const;
    // ln p of each sentence: its pieces and the end, after the start.
    // `context`: the text just before them, as the model was trained with
    // (its last pieces, then the separator); empty: none.
    [[nodiscard]] std::vector<double> LogProbabilities(std::span<const std::wstring> sentences,
                                                       std::wstring_view context = {}) const;
    // Which of `sentences` (likeliest first, as the converter ranks them)
    // to take: 0 keeps the first. Only the first Top() of the first
    // `ranked` are looked at; those after `ranked` (whole texts read with
    // other phrase breaks) count as the last rank less `penalty`. `context`:
    // the text just before them. `read` (when given): each one's log
    // probability, weighted as compared (0 for those not looked at), and
    // its prior.
    [[nodiscard]] std::size_t Choose(std::span<const std::wstring> sentences, std::size_t ranked = SIZE_MAX,
                                     double penalty = 0.0, std::wstring_view context = {},
                                     std::vector<std::pair<double, double>>* read = nullptr) const;

    // Reads many texts that start alike, as a search phrase by phrase makes
    // them: what they share is read once.
    class Reader;

private:
    struct Piece {
        std::uint32_t id;
        float score;
    };
    struct State {
        std::vector<float> h, c;
    };

    [[nodiscard]] State Start(std::wstring_view context = {}) const;
    [[nodiscard]] State Step(const State& state, std::uint32_t piece) const;
    // ln p of every piece after `state`'s, as logits less their log-sum.
    void NextLogits(const State& state, std::vector<float>& logits, float& logSum) const;

    MappedFile file_;
    std::uint32_t vocab_{0}, embed_{0}, hidden_{0};
    float scale_{0}, weight_{0}, rampChars_{0}, margin_{0}, unknownScore_{0};
    bool rampWithContext_{true};
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

class SentenceReranker::Reader final {
public:
    // `context`: the text just before them.
    Reader(const SentenceReranker& model, std::wstring_view context = {});
    // ln p of `text`'s pieces after the start, and of the end with `end`.
    [[nodiscard]] double LogProbability(std::wstring_view text, bool end);
    // Lets go of what was worked out for the pieces after each read so far
    // (the most memory); it is worked out again when needed.
    void Forget() noexcept;

private:
    struct Node {
        std::uint32_t piece{0};
        std::size_t parent{0};
        std::vector<std::size_t> children;
        double logProbability{0};
        std::optional<State> state;
        std::vector<float> logits;
        float logSum{0};
    };
    const State& StateOf(std::size_t node);

    const SentenceReranker& model_;
    std::vector<Node> nodes_;
};

}  // namespace tekito::japanese
