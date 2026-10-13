#include "Core/Japanese/SentenceReranker.h"

#include "Core/Japanese/NeuralMath.h"
#include "Core/Japanese/SortedTsv.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace tekito::japanese {
namespace {

constexpr char kMagic[4] = {'T', 'K', 'R', 'R'};
constexpr std::uint32_t kVersion = 1;
constexpr wchar_t kSpace = L'\x2581';  // SentencePiece's ▁ for a space

enum PieceType : int { kNormal = 1, kUnknown = 2, kControl = 3, kUserDefined = 4, kByte = 6 };

constexpr double kNone = -std::numeric_limits<double>::infinity();

// The best way found to read the text up to a point.
struct Best {
    double score{kNone};
    std::size_t from{0};
    std::uint32_t id{0};
    bool unknown{false};
};

std::wstring Normalized(std::wstring_view text) {
    const std::wstring nfkc = Nfkc(text);
    // No space at the ends, one between words, each written ▁, and one
    // before the text.
    std::wstring out(1, kSpace);
    bool space = false;
    for (const wchar_t c : nfkc) {
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r') {
            space = out.size() > 1;
            continue;
        }
        if (space) out += kSpace;
        space = false;
        out += c;
    }
    return out;
}

void AppendUtf8(char32_t c, std::vector<std::uint8_t>& out) {
    if (c < 0x80) {
        out.push_back(static_cast<std::uint8_t>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<std::uint8_t>(0xC0 | (c >> 6)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<std::uint8_t>(0xE0 | (c >> 12)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xF0 | (c >> 18)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
    }
}

std::size_t CodePoints(std::wstring_view text) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() && IS_LOW_SURROGATE(text[i + 1])) ++i;
        ++count;
    }
    return count;
}

}  // namespace

bool SentenceReranker::Open(const std::filesystem::path& packDirectory) {
    vocab_ = 0;
    if (!file_.Open(packDirectory / L"reranker.bin")) return false;
    const std::uint8_t* data = file_.Data();
    const std::size_t size = file_.Size();
    std::size_t at = 0;
    const auto read = [&](void* out, std::size_t bytes) {
        if (at + bytes > size) return false;
        std::memcpy(out, data + at, bytes);
        at += bytes;
        return true;
    };
    char magic[4]{};
    std::uint32_t header[5]{};
    float settings[5]{};
    if (!read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !read(header, sizeof header) ||
        header[0] != kVersion || !read(settings, sizeof settings)) {
        return false;
    }
    const std::uint32_t vocab = header[1], embed = header[2], hidden = header[3], top = header[4];
    if (vocab == 0 || embed == 0 || hidden == 0 || embed % 16 != 0 || hidden % 16 != 0 || top == 0 || top > 64) {
        return false;
    }
    prior_.resize(top);
    if (!read(prior_.data(), top * sizeof(float))) return false;
    const auto section = [&](std::size_t bytes) -> const std::uint8_t* {
        at = (at + 63) / 64 * 64;
        if (at + bytes > size) return nullptr;
        const std::uint8_t* start = data + at;
        at += bytes;
        return start;
    };
    const std::size_t gates = 4 * static_cast<std::size_t>(hidden);
    const auto* embedding = section(std::size_t{vocab} * embed);
    const auto* embeddingScale = section(std::size_t{vocab} * sizeof(float));
    const auto* inputWeights = section(gates * embed);
    const auto* inputScale = section(gates * sizeof(float));
    const auto* hiddenWeights = section(gates * hidden);
    const auto* hiddenScale = section(gates * sizeof(float));
    const auto* bias = section(gates * sizeof(float));
    const auto* projection = section(std::size_t{embed} * hidden * sizeof(float));
    const auto* projectionBias = section(std::size_t{embed} * sizeof(float));
    if (!projectionBias || at != size) return false;
    embedding_ = reinterpret_cast<const std::int8_t*>(embedding);
    embeddingScale_ = reinterpret_cast<const float*>(embeddingScale);
    inputWeights_ = reinterpret_cast<const std::int8_t*>(inputWeights);
    inputScale_ = reinterpret_cast<const float*>(inputScale);
    hiddenWeights_ = reinterpret_cast<const std::int8_t*>(hiddenWeights);
    hiddenScale_ = reinterpret_cast<const float*>(hiddenScale);
    bias_ = reinterpret_cast<const float*>(bias);
    projection_ = reinterpret_cast<const float*>(projection);
    projectionBias_ = reinterpret_cast<const float*>(projectionBias);
    scale_ = settings[0];
    weight_ = settings[1];
    rampChars_ = settings[2];
    margin_ = settings[3];
    unknownScore_ = settings[4];

    // pieces.tsv: id, type, score, piece.
    std::ifstream pieces(packDirectory / L"pieces.tsv", std::ios::binary);
    if (!pieces) return false;
    pieces_.clear();
    longestPiece_ = 0;
    std::fill(std::begin(bytePiece_), std::end(bytePiece_), 0u);
    std::size_t count = 0, bytes = 0;
    for (std::string line; std::getline(pieces, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string_view rest(line);
        std::string_view fields[3];
        for (auto& field : fields) {
            const std::size_t tab = rest.find('\t');
            if (tab == std::string_view::npos) return false;
            field = rest.substr(0, tab);
            rest.remove_prefix(tab + 1);
        }
        const auto id = static_cast<std::uint32_t>(std::strtoul(std::string(fields[0]).c_str(), nullptr, 10));
        const int type = std::atoi(std::string(fields[1]).c_str());
        const float score = std::strtof(std::string(fields[2]).c_str(), nullptr);
        if (id >= vocab) return false;
        ++count;
        if (type == kByte) {
            // <0xHH>
            if (rest.size() != 6) return false;
            bytePiece_[std::strtoul(std::string(rest.substr(3, 2)).c_str(), nullptr, 16) & 0xFF] = id;
            ++bytes;
        } else if (type == kNormal || type == kUserDefined) {
            auto piece = FromUtf8(rest);
            longestPiece_ = std::max(longestPiece_, piece.size());
            pieces_.emplace(std::move(piece), Piece{id, score});
        } else if (type == kControl && rest == "</s>") {
            end_ = id;
        }
        if (rest == "<sep>") separator_ = id;
    }
    if (count != vocab || bytes != 256) return false;
    vocab_ = vocab;
    embed_ = embed;
    hidden_ = hidden;
    return true;
}

std::vector<std::uint32_t> SentenceReranker::Pieces(std::wstring_view text) const {
    std::vector<std::uint32_t> ids;
    if (!IsOpen()) return ids;
    const std::wstring normalized = Normalized(text);
    if (normalized.size() == 1) return ids;  // nothing but spaces
    // Viterbi over code points: the pieces with the highest sum of scores;
    // a character no piece starts with is unknown, at a penalty.
    std::vector<std::size_t> starts;  // code point offsets, and the end
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        starts.push_back(i);
        if (IS_HIGH_SURROGATE(normalized[i]) && i + 1 < normalized.size() && IS_LOW_SURROGATE(normalized[i + 1])) ++i;
    }
    starts.push_back(normalized.size());
    const std::size_t n = starts.size() - 1;
    std::vector<Best> best(n + 1);
    best[0].score = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (best[i].score == kNone) continue;
        bool single = false;
        for (std::size_t j = i + 1; j <= n && starts[j] - starts[i] <= longestPiece_; ++j) {
            const auto found = pieces_.find(normalized.substr(starts[i], starts[j] - starts[i]));
            if (found == pieces_.end()) continue;
            single = single || j == i + 1;
            const double score = best[i].score + found->second.score;
            if (score > best[j].score) best[j] = {score, i, found->second.id, false};
        }
        if (!single) {
            const double score = best[i].score + unknownScore_;
            if (score > best[i + 1].score) best[i + 1] = {score, i, 0, true};
        }
    }
    std::vector<std::size_t> path;
    for (std::size_t at = n; at > 0; at = best[at].from) path.push_back(at);
    std::reverse(path.begin(), path.end());
    for (const std::size_t end : path) {
        const Best& step = best[end];
        if (!step.unknown) {
            ids.push_back(step.id);
            continue;
        }
        // An unknown character goes as its UTF-8 bytes.
        const std::size_t begin = starts[step.from];
        char32_t c = normalized[begin];
        if (starts[end] - begin == 2) c = 0x10000 + ((c - 0xD800) << 10) + (normalized[begin + 1] - 0xDC00);
        std::vector<std::uint8_t> utf8;
        AppendUtf8(c, utf8);
        for (const std::uint8_t b : utf8) ids.push_back(bytePiece_[b]);
    }
    return ids;
}

SentenceReranker::State SentenceReranker::Start() const {
    State state{std::vector<float>(hidden_, 0.0f), std::vector<float>(hidden_, 0.0f)};
    return Step(state, separator_);
}

SentenceReranker::State SentenceReranker::Step(const State& state, std::uint32_t piece) const {
    // The piece's embedding, and both inputs as int8.
    std::vector<float> x(embed_);
    const std::int8_t* row = embedding_ + std::size_t{piece} * embed_;
    for (std::uint32_t e = 0; e < embed_; ++e) x[e] = embeddingScale_[piece] * row[e];
    std::vector<std::int8_t> qx, qh;
    const float sx = Quantize(x, qx);
    const float sh = Quantize(state.h, qh);
    const std::size_t gates = 4 * std::size_t{hidden_};
    std::vector<std::int32_t> fromInput(gates), fromHidden(gates);
    DotRows(inputWeights_, gates, embed_, qx.data(), fromInput.data());
    DotRows(hiddenWeights_, gates, hidden_, qh.data(), fromHidden.data());
    std::vector<float> g(gates);
    for (std::size_t j = 0; j < gates; ++j) {
        g[j] = bias_[j] + inputScale_[j] * sx * static_cast<float>(fromInput[j]) +
               hiddenScale_[j] * sh * static_cast<float>(fromHidden[j]);
    }
    // PyTorch's order: input, forget, cell, output.
    State next{std::vector<float>(hidden_), std::vector<float>(hidden_)};
    for (std::uint32_t k = 0; k < hidden_; ++k) {
        const float input = Sigmoid(g[k]);
        const float forget = Sigmoid(g[hidden_ + k]);
        const float cell = std::tanh(g[2 * hidden_ + k]);
        const float output = Sigmoid(g[3 * hidden_ + k]);
        next.c[k] = forget * state.c[k] + input * cell;
        next.h[k] = output * std::tanh(next.c[k]);
    }
    return next;
}

void SentenceReranker::NextLogits(const State& state, std::vector<float>& logits, float& logSum) const {
    std::vector<float> projected(embed_);
    for (std::uint32_t e = 0; e < embed_; ++e) {
        const float* row = projection_ + std::size_t{e} * hidden_;
        float sum = projectionBias_[e];
        for (std::uint32_t k = 0; k < hidden_; ++k) sum += row[k] * state.h[k];
        projected[e] = sum;
    }
    std::vector<std::int8_t> q;
    const float sp = Quantize(projected, q);
    std::vector<std::int32_t> dots(vocab_);
    DotRows(embedding_, vocab_, embed_, q.data(), dots.data());
    logits.resize(vocab_);
    float largest = -std::numeric_limits<float>::infinity();
    for (std::uint32_t r = 0; r < vocab_; ++r) {
        logits[r] = embeddingScale_[r] * sp * static_cast<float>(dots[r]);
        largest = std::max(largest, logits[r]);
    }
    logSum = largest + std::log(SumExp(logits.data(), vocab_, largest));
}
std::vector<double> SentenceReranker::LogProbabilities(std::span<const std::wstring> sentences) const {
    std::vector<double> result(sentences.size(), 0.0);
    if (!IsOpen() || sentences.empty()) return result;
    // The sentences' pieces as a tree: what they share at the start is
    // read once.
    struct Node {
        std::uint32_t piece{0};
        std::vector<std::size_t> children;
        double logProbability{0};
    };
    std::vector<Node> nodes(1);
    std::vector<std::size_t> ends(sentences.size());
    for (std::size_t s = 0; s < sentences.size(); ++s) {
        auto pieces = Pieces(sentences[s]);
        pieces.push_back(end_);
        std::size_t at = 0;
        for (const std::uint32_t piece : pieces) {
            std::size_t next = 0;
            for (const std::size_t child : nodes[at].children) {
                if (nodes[child].piece == piece) next = child;
            }
            if (next == 0) {
                next = nodes.size();
                nodes[at].children.push_back(next);
                nodes.push_back({piece, {}, 0});
            }
            at = next;
        }
        ends[s] = at;
    }
    std::vector<float> logits;
    // Depth first, each node's state passed down once.
    std::vector<std::pair<std::size_t, State>> stack;
    stack.emplace_back(0, Start());
    while (!stack.empty()) {
        auto [at, state] = std::move(stack.back());
        stack.pop_back();
        float logSum = 0;
        NextLogits(state, logits, logSum);
        for (const std::size_t child : nodes[at].children) {
            nodes[child].logProbability = nodes[at].logProbability + logits[nodes[child].piece] - logSum;
            if (!nodes[child].children.empty()) stack.emplace_back(child, Step(state, nodes[child].piece));
        }
    }
    for (std::size_t s = 0; s < sentences.size(); ++s) result[s] = nodes[ends[s]].logProbability;
    return result;
}

std::size_t SentenceReranker::Choose(std::span<const std::wstring> sentences) const {
    const std::size_t count = std::min(sentences.size(), prior_.size());
    if (!IsOpen() || count < 2) return 0;
    const auto logProbabilities = LogProbabilities(sentences.first(count));
    const double ramp = rampChars_ > 0 ? std::min(1.0, CodePoints(sentences[0]) / static_cast<double>(rampChars_)) : 1.0;
    std::size_t best = 0;
    std::vector<double> scores(count);
    for (std::size_t i = 0; i < count; ++i) {
        scores[i] = weight_ * ramp * scale_ * logProbabilities[i] + prior_[i];
        if (scores[i] > scores[best]) best = i;
    }
    return scores[best] - scores[0] > margin_ ? best : 0;
}

}  // namespace tekito::japanese
