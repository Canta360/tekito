#include "Core/Japanese/EdgeScorer.h"

#include "Core/Japanese/NeuralMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace tekito::japanese {

namespace {

// As the script reads its input: padding, unknown, and the marks between
// the parts; the first characters before the caret it reads, the longest
// draft and the characters of a word.
constexpr int kUnknown = 1, kStart = 2, kAfterContext = 3, kAfterReading = 4;
constexpr std::size_t kContextChars = 40, kDraftChars = 96, kWordChars = 12;

void Norm(const float* x, const std::vector<float>& weight, const std::vector<float>& bias, std::size_t n, float* y) {
    float mean = 0, var = 0;
    for (std::size_t i = 0; i < n; ++i) mean += x[i];
    mean /= static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) var += (x[i] - mean) * (x[i] - mean);
    const float scale = 1.0f / std::sqrt(var / static_cast<float>(n) + 1e-5f);
    for (std::size_t i = 0; i < n; ++i) y[i] = (x[i] - mean) * scale * weight[i] + bias[i];
}

float Gelu(float x) { return 0.5f * x * (1.0f + std::erf(x * 0.70710678f)); }

}  // namespace

void EdgeScorer::Linear::Quantize() {
    constexpr std::size_t kLanes = 32;
    padded = (in + kLanes - 1) / kLanes * kLanes;
    quantized.assign(out * padded, 0);
    rowScale.assign(out, 0.0f);
    for (std::size_t o = 0; o < out; ++o) {
        const float* row = weight.data() + o * in;
        float largest = 0;
        for (std::size_t i = 0; i < in; ++i) largest = std::max(largest, std::abs(row[i]));
        const float scale = largest > 0 ? largest / 127.0f : 1.0f;
        rowScale[o] = scale;
        for (std::size_t i = 0; i < in; ++i) {
            quantized[o * padded + i] = static_cast<std::int8_t>(std::lround(row[i] / scale));
        }
    }
}

// In int8 (DotRows): the input to int8 with its scale, each row's sum
// scaled back.
void EdgeScorer::Linear::Apply(const float* x, float* y) const noexcept {
    thread_local std::vector<std::int8_t> input;
    thread_local std::vector<std::int32_t> sums;
    input.assign(padded, 0);
    sums.resize(out);
    float largest = 0;
    for (std::size_t i = 0; i < in; ++i) largest = std::max(largest, std::abs(x[i]));
    const float scale = largest > 0 ? largest / 127.0f : 1.0f;
    for (std::size_t i = 0; i < in; ++i) input[i] = static_cast<std::int8_t>(std::lround(x[i] / scale));
    DotRows(quantized.data(), out, padded, input.data(), sums.data());
    for (std::size_t o = 0; o < out; ++o) {
        y[o] = static_cast<float>(sums[o]) * rowScale[o] * scale + (bias.empty() ? 0.0f : bias[o]);
    }
}

bool EdgeScorer::Open(const std::filesystem::path& dir) {
    dim_ = 0;
    std::ifstream file(dir / "edges.bin", std::ios::binary);
    char magic[4];
    std::uint32_t header[9];
    if (!file.read(magic, 4) || std::memcmp(magic, "TKES", 4) != 0 || !file.read(reinterpret_cast<char*>(header), sizeof header) ||
        header[0] != 1) {
        return false;
    }
    vocab_ = header[1];
    const std::size_t dim = header[2], layers = header[3];
    heads_ = header[4];
    ff_ = header[5];
    longest_ = header[6];
    parts_ = header[7];
    part_ = header[8];
    bool good = true;
    const auto read = [&](std::vector<float>& to, std::size_t count) {
        to.resize(count);
        good = good && static_cast<bool>(file.read(reinterpret_cast<char*>(to.data()), static_cast<std::streamsize>(count * 4)));
    };
    const auto linear = [&](Linear& to, std::size_t in, std::size_t out) {
        to.in = in;
        to.out = out;
        read(to.weight, in * out);
        read(to.bias, out);
        if (good) to.Quantize();
    };
    read(chars_, vocab_ * dim);
    read(places_, longest_ * dim);
    layers_.assign(layers, {});
    for (auto& layer : layers_) {
        read(layer.norm1Weight, dim);
        read(layer.norm1Bias, dim);
        linear(layer.attention, dim, 3 * dim);
        linear(layer.projection, dim, dim);
        read(layer.norm2Weight, dim);
        read(layer.norm2Bias, dim);
        linear(layer.up, dim, ff_);
        linear(layer.down, ff_, dim);
    }
    read(normWeight_, dim);
    read(normBias_, dim);
    linear(span1_, 2 * dim, dim);
    linear(span2_, dim, dim);
    read(left_, parts_ * part_);
    read(right_, parts_ * part_);
    linear(word1_, dim + 2 * part_ + 2, dim);
    linear(word2_, dim, dim);
    std::vector<float> scale;
    read(scale, 1);
    if (!good) return false;
    costScale_ = std::exp(scale[0]);
    tokens_.clear();
    std::ifstream chars(dir / "chars.txt");
    std::string line;
    for (int index = 0; std::getline(chars, line); ++index) {
        const auto code = std::strtoul(line.c_str(), nullptr, 16);
        if (code != 0) tokens_.emplace(static_cast<wchar_t>(code), index);
    }
    dim_ = dim;
    return true;
}

int EdgeScorer::Token(wchar_t c) const noexcept {
    const auto found = tokens_.find(c);
    return found == tokens_.end() ? kUnknown : found->second;
}

std::vector<float> EdgeScorer::Score(std::wstring_view context, std::wstring_view reading, std::wstring_view draft,
                                     std::span<const Word> words) const {
    std::vector<float> scores(words.size(), 0.0f);
    if (!IsOpen() || words.empty()) return scores;
    const std::size_t d = dim_;
    // The tokens: start, the text before, a mark, the reading, a mark, the draft.
    std::vector<int> tokens{kStart};
    for (const wchar_t c : context.substr(context.size() > kContextChars ? context.size() - kContextChars : 0)) {
        tokens.push_back(Token(c));
    }
    tokens.push_back(kAfterContext);
    const std::size_t readingAt = tokens.size();
    for (const wchar_t c : reading) tokens.push_back(Token(c));
    tokens.push_back(kAfterReading);
    for (const wchar_t c : draft.substr(0, kDraftChars)) tokens.push_back(Token(c));
    if (tokens.size() > longest_) return scores;
    const std::size_t n = tokens.size();
    std::wstring key(tokens.begin(), tokens.end());
    std::vector<float> h;
    {
        std::lock_guard lock(cacheMutex_);
        for (const auto& entry : cache_) {
            if (entry.key == key) {
                h = entry.hidden;
                break;
            }
        }
    }
    if (h.empty()) {
    h.resize(n * d);
    std::vector<float> x(n * d), qkv(n * 3 * d), attended(n * d), out(d), up(ff_);
    for (std::size_t t = 0; t < n; ++t) {
        for (std::size_t i = 0; i < d; ++i) x[t * d + i] = chars_[tokens[t] * d + i] + places_[t * d + i];
    }
    const std::size_t headDim = d / heads_;
    const float attentionScale = 1.0f / std::sqrt(static_cast<float>(headDim));
    std::vector<float> weights(n);
    for (const auto& layer : layers_) {
        for (std::size_t t = 0; t < n; ++t) {
            Norm(&x[t * d], layer.norm1Weight, layer.norm1Bias, d, &h[t * d]);
            layer.attention.Apply(&h[t * d], &qkv[t * 3 * d]);
        }
        for (std::size_t head = 0; head < heads_; ++head) {
            const std::size_t q0 = head * headDim, k0 = d + head * headDim, v0 = 2 * d + head * headDim;
            for (std::size_t t = 0; t < n; ++t) {
                float peak = -1e30f;
                for (std::size_t u = 0; u < n; ++u) {
                    float dot = 0;
                    for (std::size_t i = 0; i < headDim; ++i) dot += qkv[t * 3 * d + q0 + i] * qkv[u * 3 * d + k0 + i];
                    weights[u] = dot * attentionScale;
                    peak = std::max(peak, weights[u]);
                }
                float total = 0;
                for (std::size_t u = 0; u < n; ++u) total += weights[u] = std::exp(weights[u] - peak);
                for (std::size_t i = 0; i < headDim; ++i) {
                    float sum = 0;
                    for (std::size_t u = 0; u < n; ++u) sum += weights[u] * qkv[u * 3 * d + v0 + i];
                    attended[t * d + q0 + i] = sum / total;
                }
            }
        }
        for (std::size_t t = 0; t < n; ++t) {
            layer.projection.Apply(&attended[t * d], out.data());
            for (std::size_t i = 0; i < d; ++i) x[t * d + i] += out[i];
            Norm(&x[t * d], layer.norm2Weight, layer.norm2Bias, d, &h[t * d]);
            layer.up.Apply(&h[t * d], up.data());
            for (auto& v : up) v = std::max(v, 0.0f);
            layer.down.Apply(up.data(), out.data());
            for (std::size_t i = 0; i < d; ++i) x[t * d + i] += out[i];
        }
    }
    for (std::size_t t = 0; t < n; ++t) Norm(&x[t * d], normWeight_, normBias_, d, &h[t * d]);
        constexpr std::size_t kCached = 8;
        std::lock_guard lock(cacheMutex_);
        if (cache_.size() >= kCached) cache_.erase(cache_.begin());
        cache_.push_back({std::move(key), readingAt, h});
    }
    // Each word: its span against the word itself.
    std::vector<float> spanIn(2 * d), mid(d), span(d), wordIn(word1_.in), word(d);
    const float root = std::sqrt(static_cast<float>(d));
    for (std::size_t k = 0; k < words.size(); ++k) {
        const auto& w = words[k];
        if (w.end <= w.begin || w.end > reading.size()) continue;
        std::copy_n(&h[(readingAt + w.begin) * d], d, spanIn.begin());
        std::copy_n(&h[(readingAt + w.end - 1) * d], d, spanIn.begin() + static_cast<std::ptrdiff_t>(d));
        span1_.Apply(spanIn.data(), mid.data());
        for (auto& v : mid) v = Gelu(v);
        span2_.Apply(mid.data(), span.data());
        std::fill(wordIn.begin(), wordIn.end(), 0.0f);
        const std::size_t letters = std::min(w.text.size(), kWordChars);
        for (std::size_t c = 0; c < letters; ++c) {
            const int token = Token(w.text[c]);
            for (std::size_t i = 0; i < d; ++i) wordIn[i] += chars_[token * d + i];
        }
        if (letters > 0) {
            for (std::size_t i = 0; i < d; ++i) wordIn[i] /= static_cast<float>(letters);
        }
        const std::size_t left = std::min<std::size_t>(w.left, parts_ - 1), right = std::min<std::size_t>(w.right, parts_ - 1);
        std::copy_n(&left_[left * part_], part_, wordIn.begin() + static_cast<std::ptrdiff_t>(d));
        std::copy_n(&right_[right * part_], part_, wordIn.begin() + static_cast<std::ptrdiff_t>(d + part_));
        wordIn[d + 2 * part_] = w.known ? 1.0f : 0.0f;
        wordIn[d + 2 * part_ + 1] = static_cast<float>(w.cost) / 5000.0f;
        word1_.Apply(wordIn.data(), mid.data());
        for (auto& v : mid) v = Gelu(v);
        word2_.Apply(mid.data(), word.data());
        float dot = 0;
        for (std::size_t i = 0; i < d; ++i) dot += span[i] * word[i];
        scores[k] = dot / root;
    }
    return scores;
}

}  // namespace tekito::japanese
