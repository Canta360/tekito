#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {

// English typed without spaces (seeyouonmonday): the fewest words `known`
// takes that make up all of `keys`, the better scored by `score` when as
// few make it up; nothing when no such words do. `keys`: lower-case letters.
[[nodiscard]] std::optional<std::vector<std::wstring>> SplitRunTogether(
    std::wstring_view keys, const std::function<bool(std::wstring_view)>& known,
    const std::function<double(std::wstring_view)>& score);

}  // namespace tekito
