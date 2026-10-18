#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tekito {

// Which spelling English uses (UserSettings::englishSpelling).
enum class EnglishVariety {
    // Either: words are kept as typed.
    Either = 0,
    American = 1,
    British = 2,
};

// American and British spellings of the same word (color, colour), from the
// spelling-variants pack.
class SpellingVariants final {
public:
    // The pack in the installed data; empty when it is missing.
    [[nodiscard]] static const SpellingVariants& Installed();

    // variants.tsv: "american<TAB>british" per line.
    bool Load(const std::filesystem::path& file);
    void Add(std::wstring american, std::wstring british);
    [[nodiscard]] bool Empty() const noexcept { return british_.empty(); }

    // `lower` in `variety`'s spelling, when it is the other one's (colour:
    // color for American). Nothing when it already is, or has no variant.
    [[nodiscard]] std::optional<std::wstring> InVariety(std::wstring_view lower, EnglishVariety variety) const;

private:
    std::unordered_map<std::wstring, std::wstring> british_;   // american -> british
    std::unordered_map<std::wstring, std::wstring> american_;  // british -> american
};

}  // namespace tekito
