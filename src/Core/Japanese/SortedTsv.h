#pragma once

#include "Core/Japanese/MappedFile.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// A TSV file sorted bytewise by its first field, mapped and searched in
// place: nothing is read or indexed up front, so opening it in every
// application costs nothing and a lookup touches a few pages.
class SortedTsv final {
public:
    bool Open(const std::filesystem::path& path) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return file_.Size() > 0; }
    // Calls `row` with the fields of every row whose first field is `key`,
    // in file order, until it returns false.
    void ForEachRow(std::string_view key,
                    const std::function<bool(const std::vector<std::string_view>& fields)>& row) const;

private:
    [[nodiscard]] std::size_t LineStart(std::size_t at) const noexcept;
    [[nodiscard]] std::string_view KeyAt(std::size_t lineStart) const noexcept;

    MappedFile file_;
};

[[nodiscard]] std::string ToUtf8(std::wstring_view text);
[[nodiscard]] std::wstring FromUtf8(std::string_view bytes);

}  // namespace tekito::japanese
