#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// The seven digits of a postal code typed in Japanese: 1000001 or 100-0001
// (the hyphen as Japanese input types it, ー, or any dash), half- or
// full-width. Nothing for other text.
[[nodiscard]] std::optional<std::wstring> PostalCodeDigits(std::wstring_view text);

// The japanese-zipcode pack (scripts/build-japanese-zipcode.py): the
// addresses each postal code covers, from Japan Post's data.
class PostalCodes final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return codes_.IsOpen(); }
    // The addresses for seven digits ("1000001" -> 東京都千代田区千代田), at most
    // `limit`, in the pack's order.
    [[nodiscard]] std::vector<std::wstring> Addresses(std::wstring_view digits, std::size_t limit) const;

private:
    SortedTsv codes_;
};

}  // namespace tekito::japanese
