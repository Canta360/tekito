#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace tekito::japanese {

// The japanese-homophones pack (scripts/ja/build-japanese-homophones-pack.py):
// for a word that shares its reading with others (信任, 新任), the words its
// sentences have besides more often than chance (信任: 内閣; 新任: 教師),
// as pointwise mutual information; mapped and searched in place.
class Homophones final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return table_.IsOpen(); }

    // How much more often than chance `context` is in the sentences of
    // `word`, in nats; nothing when the pack does not have the pair.
    [[nodiscard]] std::optional<double> Weight(std::wstring_view word, std::wstring_view context) const;

private:
    SortedTsv table_;
};

}  // namespace tekito::japanese
