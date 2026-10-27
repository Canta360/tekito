#include "Core/Japanese/TopicVectors.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace tekito::japanese {
namespace {

constexpr char kMagic[4] = {'T', 'K', 'T', 'V'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeader = 16;
constexpr std::size_t kRow = sizeof(float) + TopicVectors::kDims;

}  // namespace

bool TopicVectors::Open(const std::filesystem::path& packDirectory) {
    count_ = 0;
    if (!file_.Open(packDirectory / L"vectors.bin") || !words_.Open(packDirectory / L"words.tsv")) return false;
    const auto* data = file_.Data();
    if (file_.Size() < kHeader || std::memcmp(data, kMagic, sizeof kMagic) != 0) return false;
    std::uint32_t version = 0, count = 0, dims = 0;
    std::memcpy(&version, data + 4, 4);
    std::memcpy(&count, data + 8, 4);
    std::memcpy(&dims, data + 12, 4);
    if (version != kVersion || dims != kDims || file_.Size() < kHeader + static_cast<std::size_t>(count) * kRow) {
        return false;
    }
    count_ = count;
    return true;
}

std::optional<TopicVectors::Vector> TopicVectors::Of(std::wstring_view word) const {
    if (!IsOpen() || word.empty()) return std::nullopt;
    std::optional<std::uint32_t> row;
    words_.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
        if (fields.size() > 1) row = static_cast<std::uint32_t>(std::strtoul(std::string(fields[1]).c_str(), nullptr, 10));
        return false;
    });
    if (!row || *row >= count_) return std::nullopt;
    const auto* at = file_.Data() + kHeader + static_cast<std::size_t>(*row) * kRow;
    float scale = 0;
    std::memcpy(&scale, at, sizeof scale);
    const auto* values = reinterpret_cast<const std::int8_t*>(at + sizeof scale);
    Vector vector;
    for (std::size_t i = 0; i < kDims; ++i) vector[i] = static_cast<float>(values[i]) * scale;
    return vector;
}

void TopicVectors::Add(Topic& topic, std::wstring_view word) const {
    const auto vector = Of(word);
    if (!vector) return;
    for (std::size_t i = 0; i < kDims; ++i) topic.sum[i] += (*vector)[i];
    ++topic.words;
}

std::optional<double> TopicVectors::Closeness(const Topic& topic, std::wstring_view word) const {
    if (topic.words == 0) return std::nullopt;
    const auto vector = Of(word);
    if (!vector) return std::nullopt;
    double dot = 0, norm = 0;
    for (std::size_t i = 0; i < kDims; ++i) {
        dot += static_cast<double>(topic.sum[i]) * (*vector)[i];
        norm += static_cast<double>(topic.sum[i]) * topic.sum[i];
    }
    return norm > 0 ? dot / std::sqrt(norm) : std::optional<double>{};
}

}  // namespace tekito::japanese
