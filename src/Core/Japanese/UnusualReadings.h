#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>

namespace tekito::japanese {

// Words read in a way their kanji are hardly read (天 read あめ, 餌 read え):
// the japanese-jmdict pack's unusual-readings.tsv
// (scripts/ja/build-japanese-unusual-readings.py), rows of a reading and a
// word. JapaneseConverter::SetUnusualReadings makes them cost more.
class UnusualReadings final {
public:
    bool Open(const std::filesystem::path& file);
    [[nodiscard]] bool Empty() const noexcept { return pairs_.empty(); }
    // Whether any word of `reading` is listed (looked up first, cheaply).
    [[nodiscard]] bool HasReading(std::wstring_view reading) const;
    [[nodiscard]] bool Contains(std::wstring_view reading, std::wstring_view word) const;

private:
    std::unordered_set<std::wstring> readings_;
    std::unordered_set<std::wstring> pairs_;  // reading, tab, word
};

}  // namespace tekito::japanese
