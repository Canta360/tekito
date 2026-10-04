#pragma once

#include "Core/Japanese/MappedFile.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace tekito::japanese {

using ReadingCodes = std::basic_string<std::uint8_t>;
using ReadingCodesView = std::basic_string_view<std::uint8_t>;

struct DictionaryWord {
    enum class Surface : std::uint8_t { Pool, Reading, Katakana };

    std::uint16_t left{0};
    std::uint16_t right{0};
    std::uint16_t cost{0};
    Surface surface{Surface::Reading};
    // Mozc marks some entries as spelling corrections: offer them, never
    // choose them on their own.
    bool spellingCorrection{false};
    std::uint32_t surfaceOffset{0};
};

// The japanese-core dictionary (ja-dict-v1, see scripts/ja/build-japanese-packs.py),
// mapped read-only. Every read checks its bounds, so a damaged file gives
// fewer words, never a crash.
class JapaneseDictionary final {
public:
    bool Open(const std::filesystem::path& file) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return keyCount_ != 0; }
    [[nodiscard]] std::uint32_t KeyCount() const noexcept { return keyCount_; }
    // The part of speech of a character no word covers.
    [[nodiscard]] std::uint16_t UnknownId() const noexcept { return unknownId_; }

    // A character the dictionary never uses becomes 0, which no key contains.
    [[nodiscard]] ReadingCodes Encode(std::wstring_view reading) const;

    // For each key that is a prefix of `codes`, shortest first:
    // found(length, record).
    template <typename Found>
    void CommonPrefixSearch(ReadingCodesView codes, Found&& found) const;
    [[nodiscard]] std::optional<std::uint32_t> Find(ReadingCodesView codes) const;
    // The records whose key starts with `codes`, in key order, as a range
    // [first, end); empty if there are none.
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> PrefixRange(ReadingCodesView codes) const;
    // A record's key as text.
    [[nodiscard]] std::wstring KeyText(std::uint32_t record) const;
    // The words of a record, cheapest first; word(const DictionaryWord&)
    // returns nothing, or false to stop.
    template <typename Visit>
    void ForEachWord(std::uint32_t record, Visit&& word) const;
    [[nodiscard]] std::wstring Surface(const DictionaryWord& word, std::wstring_view reading) const;

private:
    [[nodiscard]] std::uint32_t RecordOffset(std::uint32_t record) const noexcept;
    // The key of a record, or an empty view if the record is out of bounds.
    [[nodiscard]] ReadingCodesView Key(std::uint32_t record) const noexcept;

    template <typename T>
    static T Read(const std::uint8_t* p) noexcept {
        T value;
        std::memcpy(&value, p, sizeof(T));
        return value;
    }

    MappedFile file_;
    std::uint32_t keyCount_{0};
    std::uint16_t unknownId_{0};
    const std::uint8_t* reverse_{nullptr};
    const std::uint8_t* chars_{nullptr};
    std::uint32_t charCount_{0};
    const std::uint8_t* index_{nullptr};
    const std::uint8_t* records_{nullptr};
    std::uint32_t recordsSize_{0};
    const std::uint8_t* surfaces_{nullptr};
    std::uint32_t surfaceUnits_{0};
};

template <typename Found>
void JapaneseDictionary::CommonPrefixSearch(ReadingCodesView codes, Found&& found) const {
    std::uint32_t lo = 0;
    std::uint32_t hi = keyCount_;
    // Keys in [lo, hi) share the first `depth` codes; narrow them to those
    // whose next code is codes[depth].
    for (std::size_t depth = 0; depth < codes.size() && lo < hi; ++depth) {
        const std::uint8_t code = codes[depth];
        if (code == 0) break;
        const auto codeAt = [&](std::uint32_t record) -> int {
            const auto key = Key(record);
            return depth < key.size() ? key[depth] : -1;
        };
        std::uint32_t first = lo, last = hi;
        while (first < last) {
            const std::uint32_t middle = first + (last - first) / 2;
            if (codeAt(middle) < code) first = middle + 1; else last = middle;
        }
        std::uint32_t end = first, limit = hi;
        while (end < limit) {
            const std::uint32_t middle = end + (limit - end) / 2;
            if (codeAt(middle) <= code) end = middle + 1; else limit = middle;
        }
        lo = first;
        hi = end;
        if (lo < hi && Key(lo).size() == depth + 1) found(depth + 1, lo);
    }
}

template <typename Visit>
void JapaneseDictionary::ForEachWord(std::uint32_t record, Visit&& word) const {
    if (record >= keyCount_) return;
    std::uint32_t at = RecordOffset(record);
    const std::uint32_t end = RecordOffset(record + 1);
    if (at >= end || end > recordsSize_) return;
    const std::uint32_t keyLength = records_[at];
    at += 1 + keyLength;
    if (at + 2 > end) return;
    const std::uint16_t count = Read<std::uint16_t>(records_ + at);
    at += 2;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (at + 5 > end) return;
        const std::uint8_t flags = records_[at++];
        DictionaryWord entry;
        entry.surface = static_cast<DictionaryWord::Surface>(flags & 3);
        entry.spellingCorrection = (flags & 4) != 0;
        entry.left = Read<std::uint16_t>(records_ + at);
        at += 2;
        entry.right = entry.left;
        if ((flags & 8) == 0) {
            if (at + 2 > end) return;
            entry.right = Read<std::uint16_t>(records_ + at);
            at += 2;
        }
        if (at + 2 > end) return;
        entry.cost = Read<std::uint16_t>(records_ + at);
        at += 2;
        if (entry.surface == DictionaryWord::Surface::Pool) {
            if (at + 3 > end) return;
            entry.surfaceOffset = static_cast<std::uint32_t>(records_[at]) |
                                  (static_cast<std::uint32_t>(records_[at + 1]) << 8) |
                                  (static_cast<std::uint32_t>(records_[at + 2]) << 16);
            at += 3;
        } else if (entry.surface != DictionaryWord::Surface::Reading &&
                   entry.surface != DictionaryWord::Surface::Katakana) {
            return;
        }
        if constexpr (std::is_same_v<decltype(word(static_cast<const DictionaryWord&>(entry))), bool>) {
            if (!word(static_cast<const DictionaryWord&>(entry))) return;
        } else {
            word(static_cast<const DictionaryWord&>(entry));
        }
    }
}

// How words join (ja-matrix-v1): the cost of one word following another,
// and whether a new phrase (bunsetsu) starts between them.
class ConnectionMatrix final {
public:
    bool Open(const std::filesystem::path& file) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return size_ != 0; }
    [[nodiscard]] std::uint32_t Size() const noexcept { return size_; }
    // Right id of the previous word (0 = the start), left id of the next
    // (0 = the end).
    [[nodiscard]] int Cost(std::uint16_t previousRight, std::uint16_t nextLeft) const noexcept;
    [[nodiscard]] bool IsBoundary(std::uint16_t previousRight, std::uint16_t nextLeft) const noexcept;

    // The cost of a pair the matrix does not know.
    static constexpr int kUnknownCost = 30000;

private:
    MappedFile file_;
    std::uint32_t size_{0};
    std::uint32_t scale_{1};
    std::uint32_t classCount_{0};
    std::uint32_t rowBytes_{0};
    const std::uint8_t* costs_{nullptr};
    const std::uint8_t* classes_{nullptr};
    const std::uint8_t* boundaries_{nullptr};
};

}  // namespace tekito::japanese
