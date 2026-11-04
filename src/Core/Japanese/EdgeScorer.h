#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito::japanese {

// The edge scorer (scripts/ja/train-japanese-edges.py): a small
// bidirectional Transformer that reads the text before the caret, the
// reading and the converter's own first choice, and scores each word of the
// lattice, a span of the reading written one way. The converter takes a
// word's score off its cost (JapaneseConverter::SetEdgeScorer).
class EdgeScorer final {
public:
    // <dir>/edges.bin and <dir>/chars.txt, as the script's export writes them.
    bool Open(const std::filesystem::path& dir);
    [[nodiscard]] bool IsOpen() const noexcept { return dim_ != 0; }

    struct Word {
        std::uint32_t begin{0};
        std::uint32_t end{0};
        std::uint16_t left{0};
        std::uint16_t right{0};
        std::int64_t cost{0};
        bool known{false};
        std::wstring_view text;
    };
    // Each word's score in nats; `context`: the text before (its last 40
    // characters are read), `draft`: the converter's first choice.
    [[nodiscard]] std::vector<float> Score(std::wstring_view context, std::wstring_view reading, std::wstring_view draft,
                                           std::span<const Word> words) const;
    // Nats per unit of the converter's costs.
    [[nodiscard]] float CostScale() const noexcept { return costScale_; }

private:
    struct Linear {
        std::size_t in{0}, out{0};
        std::vector<float> weight;  // out x in
        std::vector<float> bias;
        // The weights as int8, each row to `padded` (a multiple of 32) with
        // its scale (Quantize).
        std::size_t padded{0};
        std::vector<std::int8_t> quantized;
        std::vector<float> rowScale;
        void Quantize();
        void Apply(const float* x, float* y) const noexcept;
    };
    struct Layer {
        std::vector<float> norm1Weight, norm1Bias, norm2Weight, norm2Bias;
        Linear attention, projection, up, down;
    };
    [[nodiscard]] int Token(wchar_t c) const noexcept;

    std::size_t vocab_{0}, dim_{0}, heads_{0}, ff_{0}, longest_{0}, parts_{0}, part_{0};
    std::vector<float> chars_, places_, normWeight_, normBias_, left_, right_;
    std::vector<Layer> layers_;
    Linear span1_, span2_, word1_, word2_;
    float costScale_{0};
    std::unordered_map<wchar_t, int> tokens_;
    // The encoder's last outputs: a conversion asks for the same reading
    // again and again (its phrases, its sentences).
    struct Encoded {
        std::wstring key;
        std::size_t readingAt{0};
        std::vector<float> hidden;
    };
    mutable std::mutex cacheMutex_;
    mutable std::vector<Encoded> cache_;
};

}  // namespace tekito::japanese
