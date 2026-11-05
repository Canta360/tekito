#include "Core/Japanese/Notation.h"

#include "Core/Japanese/SortedTsv.h"

#include <charconv>
#include <fstream>
#include <string>

namespace tekito::japanese {
namespace {

constexpr std::uint8_t kKanjiForm = 8;

std::wstring Key(std::uint16_t left, std::wstring_view word) {
    std::wstring key(1, static_cast<wchar_t>(left));
    key += word;
    return key;
}

}  // namespace

bool Notation::Open(const std::filesystem::path& packDirectory) {
    words_.clear();
    parts_.assign(65536, false);
    std::ifstream input(packDirectory / L"notation.tsv", std::ios::binary);
    if (!input) return false;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // left TAB word TAB level TAB K or k
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        const auto c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos || c + 1 >= line.size()) continue;
        unsigned left = 0;
        int level = 0;
        if (std::from_chars(line.data(), line.data() + a, left).ec != std::errc{} || left > 0xFFFF) continue;
        if (std::from_chars(line.data() + b + 1, line.data() + c, level).ec != std::errc{}) continue;
        if (level < kLowest || level > kHighest) continue;
        const bool kanji = line[c + 1] == 'K';
        words_[Key(static_cast<std::uint16_t>(left), FromUtf8(std::string_view(line).substr(a + 1, b - a - 1)))] =
            static_cast<std::uint8_t>(level + (kanji ? kKanjiForm : 0));
        parts_[left] = true;
    }
    return IsOpen();
}

std::int64_t Notation::Cost(std::uint16_t left, std::wstring_view word, int level, std::int64_t cost) const {
    if (!Has(left)) return 0;
    const auto found = words_.find(Key(left, word));
    if (found == words_.end()) return 0;
    const int at = found->second & 7;
    const bool kanji = (found->second & kKanjiForm) != 0;
    if (kanji) return at <= level ? cost : 0;
    // More kanji than the default asks the kana forms of the words above
    // the level to cost more too.
    return level < kDefault && at > level ? cost : 0;
}

}  // namespace tekito::japanese
