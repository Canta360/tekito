#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace tekito {

// The folder of the Data Pack `packId` under `root`: `root\packId` as
// TEKITO installs them, or `root\en\packId`, `root\ja\packId` or
// `root\common\packId` as the repository keeps them, by language. When it
// is in none of them, `root\packId`, where a new pack goes.
[[nodiscard]] inline std::filesystem::path FindDataPack(const std::filesystem::path& root,
                                                        std::wstring_view packId) {
    if (root.empty()) return {};
    auto installed = root / packId;
    std::error_code error;
    if (std::filesystem::is_directory(installed, error)) return installed;
    for (const wchar_t* language : {L"en", L"ja", L"common"}) {
        auto grouped = root / language / packId;
        if (std::filesystem::is_directory(grouped, error)) return grouped;
    }
    return installed;
}

}  // namespace tekito
