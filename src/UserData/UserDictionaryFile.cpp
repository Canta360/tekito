#include "UserData/UserDictionaryFile.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace tekito::userdata {
namespace {

constexpr std::wstring_view kHeader = L"TEKITO_USER_DICTIONARY_V1";

std::wstring Hex(std::wstring_view value) {
    std::wostringstream output;
    output << std::uppercase << std::hex << std::setfill(L'0');
    for (const wchar_t ch : value) output << std::setw(4) << static_cast<unsigned int>(ch);
    return output.str();
}

bool HexValue(wchar_t value, unsigned int& result) {
    if (value >= L'0' && value <= L'9') result = static_cast<unsigned int>(value - L'0');
    else if (value >= L'a' && value <= L'f') result = static_cast<unsigned int>(value - L'a' + 10);
    else if (value >= L'A' && value <= L'F') result = static_cast<unsigned int>(value - L'A' + 10);
    else return false;
    return true;
}

bool Unhex(std::wstring_view value, std::wstring& result) {
    if (value.size() % 4 != 0) return false;
    result.clear();
    result.reserve(value.size() / 4);
    for (std::size_t index = 0; index < value.size(); index += 4) {
        unsigned int code = 0;
        for (std::size_t offset = 0; offset < 4; ++offset) {
            unsigned int digit = 0;
            if (!HexValue(value[index + offset], digit)) return false;
            code = (code << 4) | digit;
        }
        result.push_back(static_cast<wchar_t>(code));
    }
    return true;
}

bool ParseLine(std::wstring_view line, UserDictionaryEntry& entry) {
    std::wstring_view fields[6];
    for (auto& field : fields) {
        const auto separator = line.find(L'\t');
        field = separator == std::wstring_view::npos ? line : line.substr(0, separator);
        line = separator == std::wstring_view::npos ? std::wstring_view{} : line.substr(separator + 1);
    }
    if (fields[0].empty() || fields[3].empty() || fields[4].empty() || fields[5].empty()) return false;
    try {
        entry.id = std::stoull(std::wstring(fields[0]));
        if (!Unhex(fields[1], entry.raw) || !Unhex(fields[2], entry.candidate)) return false;
        entry.policyFlags = static_cast<std::uint32_t>(std::stoul(std::wstring(fields[3])));
        entry.caseSensitive = fields[4] == L"1";
        entry.enabled = fields[5] == L"1";
        return fields[4] == L"0" || fields[4] == L"1";
    } catch (...) {
        return false;
    }
}

}  // namespace

bool ExportUserDictionary(const UserDictionary& dictionary,
                          const std::filesystem::path& path) noexcept {
    try {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::wofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << kHeader << L'\n'
               << L"id\traw_hex\tcandidate_hex\tpolicy_flags\tcase_sensitive\tenabled\n";
        for (const auto& entry : dictionary.Entries()) {
            output << entry.id << L'\t' << Hex(entry.raw) << L'\t' << Hex(entry.candidate) << L'\t'
                   << entry.policyFlags << L'\t' << (entry.caseSensitive ? 1 : 0) << L'\t'
                   << (entry.enabled ? 1 : 0) << L'\n';
        }
        return static_cast<bool>(output);
    } catch (...) {
        return false;
    }
}

bool ImportUserDictionary(const std::filesystem::path& path,
                          UserDictionary& dictionary) noexcept {
    try {
        std::wifstream input(path, std::ios::binary);
        std::wstring line;
        if (!input || !std::getline(input, line) || line != kHeader || !std::getline(input, line)) {
            return false;
        }
        UserDictionary loaded;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            UserDictionaryEntry entry;
            if (!ParseLine(line, entry) || !loaded.Add(std::move(entry))) return false;
        }
        if (!input.eof()) return false;
        dictionary = std::move(loaded);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace tekito::userdata
