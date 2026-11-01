#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tekito::japanese {

// The written-forms pack (scripts/en/written-form-seeds.tsv): how a word
// typed in small letters is written by its own name (ram: RAM, asrock:
// ASRock, nvidia: NVIDIA, iphone: iPhone). Some are plain English words too
// (line, booth, ram): written so only in Japanese text, where a Latin word
// is mostly a name.
class WrittenForms final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return table_.IsOpen(); }

    struct Form {
        std::wstring written;
        // Written so only among Japanese (LINE, RAM), not in English.
        bool japaneseOnly{false};
    };
    // `lower`: small letters.
    [[nodiscard]] std::optional<Form> Find(std::wstring_view lower) const;

private:
    SortedTsv table_;
};

}  // namespace tekito::japanese
