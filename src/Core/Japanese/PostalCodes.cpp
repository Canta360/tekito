#include "Core/Japanese/PostalCodes.h"

namespace tekito::japanese {
namespace {

// A digit as ASCII, or 0.
wchar_t Digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c;
    if (c >= L'\xFF10' && c <= L'\xFF19') return static_cast<wchar_t>(c - 0xFF10 + L'0');
    return 0;
}

bool IsDash(wchar_t c) {
    return c == L'-' || c == L'\x30FC' || c == L'\xFF0D' || c == L'\x2010' || c == L'\x2212';
}

}  // namespace

std::optional<std::wstring> PostalCodeDigits(std::wstring_view text) {
    std::wstring digits;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (const wchar_t digit = Digit(text[i])) {
            digits.push_back(digit);
        } else if (!(IsDash(text[i]) && i == 3 && digits.size() == 3)) {
            return std::nullopt;
        }
    }
    if (digits.size() != 7) return std::nullopt;
    return digits;
}

bool PostalCodes::Open(const std::filesystem::path& packDirectory) noexcept {
    return codes_.Open(packDirectory / L"zipcodes.tsv");
}

std::vector<std::wstring> PostalCodes::Addresses(std::wstring_view digits, std::size_t limit) const {
    std::vector<std::wstring> addresses;
    if (!IsOpen() || digits.size() != 7 || limit == 0) return addresses;
    try {
        codes_.ForEachRow(ToUtf8(digits), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 2 && !fields[1].empty()) addresses.push_back(FromUtf8(fields[1]));
            return addresses.size() < limit;
        });
    } catch (...) {
        addresses.clear();
    }
    return addresses;
}

}  // namespace tekito::japanese
