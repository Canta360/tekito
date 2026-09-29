#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <string>

namespace tekito::userdata {

// The japanese-zipcode pack, which the user adds in Settings from Japan
// Post's postal code data. It is not shipped: Japan Post updates the data
// monthly, and only people who want addresses need it.
struct PostalCodeData {
    std::size_t count{0};
    std::wstring version;  // yyyy.mm, when the data was downloaded
};

enum class PostalCodeImportError {
    None,
    Unreadable,      // not found, or the zip could not be opened
    ShiftJis,        // Japan Post's other (Shift_JIS) download
    NotPostalCodes,  // some other file
    WriteFailed,
};

// What is installed in `packDirectory`, if anything.
[[nodiscard]] std::optional<PostalCodeData> InstalledPostalCodes(const std::filesystem::path& packDirectory);

// Builds the pack in `packDirectory` from utf_ken_all.zip as downloaded
// (https://www.post.japanpost.jp/zipcode/dl/utf-zip.html) or the CSV in it,
// replacing what was there. One row per code and address, sorted bytewise:
// "1000001 <TAB> the prefecture, city and town".
[[nodiscard]] std::optional<PostalCodeData> ImportPostalCodes(const std::filesystem::path& source,
                                                              const std::filesystem::path& packDirectory,
                                                              PostalCodeImportError& error);

// Builds the pack from the CSV's text (for tests and ImportPostalCodes).
[[nodiscard]] std::optional<PostalCodeData> BuildPostalCodePack(std::string_view csvUtf8, std::wstring version,
                                                                const std::filesystem::path& packDirectory,
                                                                PostalCodeImportError& error);

bool RemovePostalCodes(const std::filesystem::path& packDirectory);

}  // namespace tekito::userdata
