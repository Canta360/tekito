#pragma once

#include <filesystem>
#include <string>

namespace tekito::userdata {

struct DataPackStatus {
    bool valid{false};
    bool fileChecksumValid{false};
    bool indexChecksumValid{false};
    std::wstring packId;
    std::wstring displayName;
    std::wstring version;
    std::wstring type;
    std::wstring source;
    std::wstring license;
    std::filesystem::path packPath;
    std::filesystem::path noticePath;
    std::wstring reason;
};

// A file's SHA-256 in upper-case hex, as manifests give it; empty if it
// cannot be read.
[[nodiscard]] std::string FileSha256(const std::filesystem::path& path);

[[nodiscard]] bool ValidateDataPack(const std::filesystem::path& packPath,
                                    DataPackStatus& status) noexcept;

}  // namespace tekito::userdata
