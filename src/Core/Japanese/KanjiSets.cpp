#include "Core/Japanese/KanjiSets.h"

#include <fstream>
#include <string>

namespace tekito::japanese {
namespace {

bool IsKanji(char32_t c) noexcept {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0x20000 && c <= 0x3134F);
}

// The first code point of UTF-8 `bytes`.
char32_t FirstCodePoint(std::string_view bytes) noexcept {
    if (bytes.empty()) return 0;
    const auto b = static_cast<unsigned char>(bytes[0]);
    const int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    char32_t c = extra == 3 ? b & 0x07 : extra == 2 ? b & 0x0F : extra == 1 ? b & 0x1F : b;
    for (int i = 1; i <= extra && i < static_cast<int>(bytes.size()); ++i) {
        c = (c << 6) | (static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]) & 0x3F);
    }
    return c;
}

// The code points of UTF-8 `bytes`.
std::u32string CodePoints(std::string_view bytes) {
    std::u32string points;
    for (std::size_t i = 0; i < bytes.size();) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        const std::size_t length = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        points += FirstCodePoint(bytes.substr(i, length));
        i += length;
    }
    return points;
}

void AppendUtf16(std::wstring& text, char32_t c) {
    if (c >= 0x10000) {
        text += static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10));
        text += static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
    } else {
        text += static_cast<wchar_t>(c);
    }
}

}  // namespace

bool KanjiSets::Load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    for (std::string line; std::getline(input, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size()) continue;
        Add(FirstCodePoint(std::string_view(line).substr(0, tab)), std::stoi(line.substr(tab + 1)));
    }
    return !Empty();
}

bool KanjiSets::LoadVariants(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    for (std::string line; std::getline(input, line);) {
        const auto tab = line.find('	');
        if (line.empty() || line[0] == '#' || tab == std::string::npos || tab + 1 >= line.size()) continue;
        AddVariants(FirstCodePoint(std::string_view(line).substr(0, tab)), CodePoints(std::string_view(line).substr(tab + 1)));
    }
    return HasVariants();
}

std::vector<std::wstring> KanjiSets::VariantForms(std::wstring_view text, std::size_t most) const {
    std::vector<std::wstring> forms;
    if (variants_.empty()) return forms;
    for (std::size_t i = 0; i < text.size() && forms.size() < most; ++i) {
        char32_t c = text[i];
        std::size_t width = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            width = 2;
        }
        if (const auto found = variants_.find(c); found != variants_.end()) {
            for (const char32_t variant : found->second) {
                if (forms.size() >= most) break;
                std::wstring form(text.substr(0, i));
                AppendUtf16(form, variant);
                form += text.substr(i + width);
                forms.push_back(std::move(form));
            }
        }
        i += width - 1;
    }
    return forms;
}

bool KanjiSets::Allows(std::wstring_view text, KanjiRange range, int grade, bool names) const {
    if (range == KanjiRange::All || Empty()) return true;
    const int highest = range == KanjiRange::Kyoiku ? grade : range == KanjiRange::Joyo ? 8 : 9;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        }
        if (!IsKanji(c)) continue;
        const auto found = levels_.find(c);
        if (found == levels_.end()) return false;
        if (found->second > highest && !(names && found->second == 9)) return false;
    }
    return true;
}

}  // namespace tekito::japanese
