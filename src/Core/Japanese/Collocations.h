#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace tekito::japanese {

// The japanese-collocations pack (scripts/ja/build-japanese-collocations-pack.py):
// how often a run of kanji or katakana stands before a particle and another
// after it (エラー を 直, 少年誌 に 連載), mapped and searched in place.
class Collocations final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return table_.IsOpen() && total_ > 0; }

    // How much likelier than chance `before` `particle` `after` is, in nats
    // (pointwise mutual information): from the pair's count when it was
    // seen; when it never was (or too seldom for the pack to keep it), as if
    // seen once, which is below chance only for common sides; nothing when
    // the pack knows too little of either side.
    [[nodiscard]] std::optional<double> Fit(std::wstring_view before, wchar_t particle, std::wstring_view after) const;

private:
    [[nodiscard]] double Count(const std::string& key) const;

    SortedTsv table_;
    double total_{0};
};

}  // namespace tekito::japanese
