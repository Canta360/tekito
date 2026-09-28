#include "Core/Japanese/LanguageModel.h"

#include <cmath>
#include <cstring>
#include <string>

namespace tekito::japanese {
namespace {

constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ULL;
constexpr std::uint32_t kVersion = 3;
// Pair scores are round((PMI + 5) * 10); word scores round(ln(count) * 10).
constexpr double kPmiFloor = -5.0;

struct Header {
    char magic[4];
    std::uint32_t version;
    std::uint32_t pairSlots;
    std::uint32_t pairsOffset;
    std::uint32_t wordSlots;
    std::uint32_t wordsOffset;
    std::uint32_t pairs;
    std::uint32_t size;
    std::uint32_t tokens;
    std::uint32_t topicSlots;
    std::uint32_t topicsOffset;
};
static_assert(sizeof(Header) == 44);

bool IsPowerOfTwo(std::uint32_t value) noexcept { return value != 0 && (value & (value - 1)) == 0; }

}  // namespace

bool LanguageModel::Open(const std::filesystem::path& packDirectory) noexcept {
    pairSlots_ = 0;
    std::error_code error;
    if (!std::filesystem::is_regular_file(packDirectory / L"lm.bin", error)) return false;
    if (!file_.Open(packDirectory / L"lm.bin") || file_.Size() < sizeof(Header)) return false;
    Header header{};
    std::memcpy(&header, file_.Data(), sizeof(Header));
    // Every table must be where the header says and fit in the file.
    const auto fits = [&](std::uint32_t offset, std::uint32_t slots) {
        return IsPowerOfTwo(slots) && offset >= sizeof(Header) &&
               static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(slots) * 5 <= file_.Size();
    };
    if (std::memcmp(header.magic, "TKLM", 4) != 0 || header.version != kVersion || header.size != file_.Size() ||
        header.tokens == 0 || !fits(header.pairsOffset, header.pairSlots) ||
        !fits(header.wordsOffset, header.wordSlots) || !fits(header.topicsOffset, header.topicSlots)) {
        file_.Close();
        return false;
    }
    pairSlots_ = header.pairSlots;
    pairsOffset_ = header.pairsOffset;
    wordSlots_ = header.wordSlots;
    wordsOffset_ = header.wordsOffset;
    topicSlots_ = header.topicSlots;
    topicsOffset_ = header.topicsOffset;
    logTokens_ = std::log(static_cast<double>(header.tokens));
    return true;
}

std::uint64_t LanguageModel::Continue(std::uint64_t hash, std::wstring_view text) noexcept {
    for (const wchar_t unit : text) {
        const auto code = static_cast<std::uint16_t>(unit);
        hash = (hash ^ (code & 0xFF)) * kFnvPrime;
        hash = (hash ^ (code >> 8)) * kFnvPrime;
    }
    return hash;
}

std::uint64_t LanguageModel::Hash(std::wstring_view text) noexcept { return Continue(kFnvOffset, text); }

std::uint64_t LanguageModel::PairStart(std::wstring_view left) noexcept {
    return Continue(Hash(left), std::wstring_view(L"\x0001", 1));
}

std::uint8_t LanguageModel::Find(std::uint64_t hash, std::uint32_t slots, std::size_t offset) const noexcept {
    const std::uint32_t fingerprint = static_cast<std::uint32_t>(hash >> 32) | 1U;
    const std::uint8_t* keys = file_.Data() + offset;
    const std::uint8_t* scores = keys + static_cast<std::size_t>(slots) * 4;
    for (std::uint32_t at = static_cast<std::uint32_t>(hash) & (slots - 1), probes = 0; probes < slots;
         at = (at + 1) & (slots - 1), ++probes) {
        std::uint32_t key = 0;
        std::memcpy(&key, keys + static_cast<std::size_t>(at) * 4, 4);
        if (key == 0) return 0;
        if (key == fingerprint) return scores[at];
    }
    return 0;
}

std::optional<double> LanguageModel::LogCount(std::wstring_view word) const noexcept {
    const std::uint8_t score = IsOpen() ? Find(Hash(word), wordSlots_, wordsOffset_) : 0;
    if (score == 0) return std::nullopt;
    return score / 10.0;
}

std::optional<double> LanguageModel::Topic(std::wstring_view first, std::wstring_view second) const {
    if (!IsOpen() || first == second) return std::nullopt;
    if (second < first) std::swap(first, second);
    std::wstring key(first);
    key += L'\x0002';
    key += second;
    const std::uint8_t score = Find(Hash(key), topicSlots_, topicsOffset_);
    if (score == 0) return std::nullopt;
    return score / 20.0;
}

bool LanguageModel::IsContentWord(std::wstring_view word) noexcept {
    if (word.size() < 2) return false;
    for (const wchar_t c : word) {
        if ((c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005 || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC) return true;
    }
    return false;
}

std::optional<double> LanguageModel::PairPmi(std::uint64_t pairStart, std::wstring_view right) const noexcept {
    const std::uint8_t score = IsOpen() ? Find(Continue(pairStart, right), pairSlots_, pairsOffset_) : 0;
    if (score == 0) return std::nullopt;
    return score / 10.0 + kPmiFloor;
}

}  // namespace tekito::japanese
