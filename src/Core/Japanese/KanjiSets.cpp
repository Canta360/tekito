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

}  // namespace

bool KanjiSets::Load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    for (std::string line; std::getline(input, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size()) continue;
        Add(FirstCodePoint(std::string_view(line).substr(0, tab)), line[tab + 1] - '0');
    }
    return !Empty();
}

bool KanjiSets::Allows(std::wstring_view text, KanjiRange range) const {
    if (range == KanjiRange::All || Empty()) return true;
    const int highest = range == KanjiRange::Kyoiku ? 1 : range == KanjiRange::Joyo ? 2 : 3;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        }
        if (!IsKanji(c)) continue;
        const auto found = levels_.find(c);
        if (found == levels_.end() || found->second > highest) return false;
    }
    return true;
}

}  // namespace tekito::japanese
