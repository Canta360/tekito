#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

    // A row the user changed in Settings: `input` writes `output` (keeping
    // `pending`), or with no output the pack's row is not used.
    struct Change {
        std::wstring input;
        std::wstring output;
        std::wstring pending;
    };
    // The pack's rows with `changes` over them, in place of what was there
    // (each call starts again from the pack's rows).
    void ApplyChanges(const std::vector<Change>& changes);
    // The pack's own row for `input`, whatever the user changed.
    [[nodiscard]] const RomajiRule* PackRule(std::wstring_view input) const;
    // What makes `table` (rows of another IME's romaji table) out of the
    // pack's rows: its rows that differ, and, when it is a whole table,
    // the pack's rows it does not have, taken out.
    [[nodiscard]] std::vector<Change> ChangesFor(const std::vector<Change>& table) const;
    // The user's changes kept in a file: input, output, pending per line.
    [[nodiscard]] static std::vector<Change> ReadChanges(const std::filesystem::path& file);
    [[nodiscard]] static bool WriteChanges(const std::filesystem::path& file, const std::vector<Change>& changes);
    // Rows of a romaji table another IME saved (Microsoft IME, Google
    // Japanese Input): input, output and maybe pending, split by tabs, in
    // UTF-8, UTF-16 or Shift_JIS.
    [[nodiscard]] static std::vector<Change> ParseTable(const std::filesystem::path& file);

private:
    void Index();

    std::map<std::wstring, RomajiRule, std::less<>> rules_;
    std::map<std::wstring, RomajiRule, std::less<>> packRules_;
    std::map<std::wstring, std::wstring, std::less<>> keysForKana_;
};

}  // namespace tekito::japanese
