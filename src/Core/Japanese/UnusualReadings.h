#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace tekito::japanese {

// Words read in a way their kanji are hardly read (餌 read え, 工夫 read
// こうふ), or read as only old Japanese read them while they are read
// otherwise today (天 read あめ): the japanese-jmdict pack's
// unusual-readings.tsv (scripts/ja/build-japanese-unusual-readings.py), rows
// of a reading, a word and "rare" or "archaic".
// JapaneseConverter::SetUnusualReadings makes them cost more.
class UnusualReadings final {
public:
    enum class Kind { Rare, Archaic };

    bool Open(const std::filesystem::path& file);
    [[nodiscard]] bool Empty() const noexcept { return pairs_.empty(); }
    // Whether any word of `reading` is listed (looked up first, cheaply).
    [[nodiscard]] bool HasReading(std::wstring_view reading) const;
    [[nodiscard]] std::optional<Kind> Find(std::wstring_view reading, std::wstring_view word) const;
    [[nodiscard]] bool Contains(std::wstring_view reading, std::wstring_view word) const {
        return Find(reading, word).has_value();
    }

private:
    std::unordered_set<std::wstring> readings_;
    std::unordered_map<std::wstring, Kind> pairs_;  // reading, tab, word
};

}  // namespace tekito::japanese
