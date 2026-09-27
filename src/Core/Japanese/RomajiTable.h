#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace tekito::japanese {

struct RomajiRule {
    std::wstring output;
    // Keys that stay pending as the start of the next input ("kk" -> "っ",
    // pending "k").
    std::wstring pending;
};

// The japanese-romaji pack: which typed keys become which kana. The table is
// a few hundred rows, so it is read whole.
class RomajiTable final {
public:
    // Rows of "input<TAB>output<TAB>pending"; malformed rows are skipped.
    [[nodiscard]] static RomajiTable FromTsv(std::wstring_view text);
    // Reads romaji.tsv from the pack directory; nullptr if it is missing.
    [[nodiscard]] static std::unique_ptr<RomajiTable> Load(const std::filesystem::path& packDirectory);

    [[nodiscard]] const RomajiRule* Find(std::wstring_view keys) const;
    // Whether some input longer than `keys` starts with it, so more keys
    // could still complete a row.
    [[nodiscard]] bool HasLongerInput(std::wstring_view keys) const;
    // The shortest keys that write exactly `kana`, or empty if none do.
    [[nodiscard]] std::wstring KeysFor(std::wstring_view kana) const;
    [[nodiscard]] bool Empty() const noexcept { return rules_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return rules_.size(); }

private:
    std::map<std::wstring, RomajiRule, std::less<>> rules_;
    std::map<std::wstring, std::wstring, std::less<>> keysForKana_;
};

}  // namespace tekito::japanese
