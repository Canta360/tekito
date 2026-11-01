#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// UTF-8 to and from std::wstring without the Windows API. A wstring holds
// UTF-16 where wchar_t is 16 bits (Windows) and code points where it is 32
// bits (Linux), as the platform's wide strings do.
namespace tekito::utf8 {

inline void AppendCodePoint(std::wstring& out, char32_t c) {
    if constexpr (sizeof(wchar_t) == 2) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
            return;
        }
    }
    out.push_back(static_cast<wchar_t>(c));
}

// Nothing when `bytes` is not valid UTF-8.
inline std::optional<std::wstring> Decode(std::string_view bytes) {
    std::wstring out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        char32_t c = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            c = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            c = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            c = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            c = lead & 0x07;
            extra = 3;
        } else {
            return std::nullopt;
        }
        if (extra > 0 && i + extra >= bytes.size()) return std::nullopt;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if ((next & 0xC0) != 0x80) return std::nullopt;
            c = (c << 6) | (next & 0x3F);
        }
        constexpr char32_t kSmallest[] = {0, 0x80, 0x800, 0x10000};
        if (c < kSmallest[extra] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return std::nullopt;
        AppendCodePoint(out, c);
        i += extra + 1;
    }
    return out;
}

// Invalid bytes become U+FFFD, as MultiByteToWideChar without
// MB_ERR_INVALID_CHARS does.
inline std::wstring DecodeLenient(std::string_view bytes) {
    if (auto strict = Decode(bytes)) return *std::move(strict);
    std::wstring out;
    for (std::size_t i = 0; i < bytes.size();) {
        std::size_t length = 1;
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        if (i + length <= bytes.size()) {
            if (auto one = Decode(bytes.substr(i, length))) {
                out += *one;
                i += length;
                continue;
            }
        }
        out.push_back(static_cast<wchar_t>(0xFFFD));
        ++i;
    }
    return out;
}

inline std::string Encode(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = static_cast<char32_t>(text[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
                const auto low = static_cast<char32_t>(text[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                }
            }
        }
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;  // a lone surrogate
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// Calls `unit` with each UTF-16 code unit of `text`, so data hashed or
// counted in UTF-16 on Windows comes out the same where wchar_t is 32 bits.
template <typename F>
inline void ForEachUtf16(std::wstring_view text, F&& unit) {
    for (const wchar_t w : text) {
        const auto c = static_cast<char32_t>(w);
        if (sizeof(wchar_t) == 4 && c >= 0x10000) {
            unit(static_cast<std::uint16_t>(0xD800 + ((c - 0x10000) >> 10)));
            unit(static_cast<std::uint16_t>(0xDC00 + ((c - 0x10000) & 0x3FF)));
        } else {
            unit(static_cast<std::uint16_t>(c));
        }
    }
}

}  // namespace tekito::utf8
