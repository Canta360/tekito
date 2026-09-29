#include "Core/SpecialConversions.h"

#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/SortedTsv.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <ctime>
#include <fstream>
#include <sstream>

namespace tekito {
namespace {

constexpr std::size_t kLongestKanjiNumber = 20;  // up to 京
constexpr int kLargestRoman = 3999;
constexpr int kLargestCircled = 50;

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const auto tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) return fields;
        start = tab + 1;
    }
}

std::wstring_view LanguageCode(SpecialConversions::Language language) {
    return language == SpecialConversions::Language::Japanese ? L"ja" : L"en";
}

std::wstring RowKey(std::wstring_view section, std::wstring_view language, std::wstring_view key) {
    std::wstring out;
    out.reserve(section.size() + language.size() + key.size() + 2);
    out.append(section).push_back(L'\t');
    out.append(language).push_back(L'\t');
    out.append(key);
    return out;
}

std::wstring Padded(int value) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02d", value);
    return buffer;
}

void AddUnique(std::vector<std::wstring>& list, std::wstring text) {
    if (!text.empty() && std::find(list.begin(), list.end(), text) == list.end()) list.push_back(std::move(text));
}

// Full-width digits as ASCII; nothing if anything else is in `text`.
std::optional<std::wstring> AsciiDigits(std::wstring_view text) {
    std::wstring digits;
    for (const wchar_t c : text) {
        if (c >= L'0' && c <= L'9') {
            digits.push_back(c);
        } else if (c >= L'\xFF10' && c <= L'\xFF19') {
            digits.push_back(static_cast<wchar_t>(c - 0xFF10 + L'0'));
        } else {
            return std::nullopt;
        }
    }
    if (digits.empty()) return std::nullopt;
    return digits;
}

std::wstring WithCommas(const std::wstring& digits) {
    std::wstring out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(L',');
        out.push_back(digits[i]);
    }
    return out;
}

std::wstring Roman(int value) {
    // Ⅰ-Ⅻ are single characters; larger numbers are spelled with Ⅰ Ⅴ Ⅹ Ⅼ Ⅽ Ⅾ Ⅿ.
    if (value >= 1 && value <= 12) return std::wstring(1, static_cast<wchar_t>(0x2160 + value - 1));
    static constexpr struct {
        int value;
        const wchar_t* text;
    } kParts[] = {
        {1000, L"\x216F"}, {900, L"\x216D\x216F"}, {500, L"\x216E"}, {400, L"\x216D\x216E"},
        {100, L"\x216D"},  {90, L"\x2169\x216D"},  {50, L"\x216C"},  {40, L"\x2169\x216C"},
        {10, L"\x2169"},   {9, L"\x2160\x2169"},   {5, L"\x2164"},   {4, L"\x2160\x2164"},
        {1, L"\x2160"},
    };
    std::wstring out;
    for (const auto& part : kParts) {
        while (value >= part.value) {
            out += part.text;
            value -= part.value;
        }
    }
    return out;
}

std::wstring Circled(int value) {
    if (value == 0) return L"\x24EA";
    if (value <= 20) return std::wstring(1, static_cast<wchar_t>(0x2460 + value - 1));
    if (value <= 35) return std::wstring(1, static_cast<wchar_t>(0x3251 + value - 21));
    return std::wstring(1, static_cast<wchar_t>(0x32B1 + value - 36));
}

// Arithmetic over + - * / and parentheses, by recursive descent. Spaces
// may stand between numbers and operators, not inside a number.
class Arithmetic final {
public:
    explicit Arithmetic(std::wstring_view text) : text_(text) {}

    std::optional<double> Evaluate() {
        const auto value = Sum();
        if (!value || Peek() != 0 || operators_ == 0) return std::nullopt;
        return value;
    }

private:
    // The next character after any spaces, or 0 at the end.
    wchar_t Peek() {
        while (at_ < text_.size() && text_[at_] == L' ') ++at_;
        return at_ < text_.size() ? text_[at_] : 0;
    }

    std::optional<double> Sum() {
        auto value = Product();
        while (value && (Peek() == L'+' || Peek() == L'-')) {
            const wchar_t op = text_[at_++];
            ++operators_;
            const auto right = Product();
            if (!right) return std::nullopt;
            *value = op == L'+' ? *value + *right : *value - *right;
        }
        return value;
    }

    std::optional<double> Product() {
        auto value = Factor();
        while (value && (Peek() == L'*' || Peek() == L'/')) {
            const wchar_t op = text_[at_++];
            ++operators_;
            const auto right = Factor();
            if (!right) return std::nullopt;
            if (op == L'/' && *right == 0.0) return std::nullopt;
            *value = op == L'*' ? *value * *right : *value / *right;
        }
        return value;
    }

    std::optional<double> Factor() {
        const wchar_t next = Peek();
        if (next == 0) return std::nullopt;
        if (next == L'-' || next == L'+') {
            ++at_;
            const auto value = Factor();
            if (!value) return std::nullopt;
            return next == L'-' ? -*value : *value;
        }
        if (next == L'(') {
            ++at_;
            const auto value = Sum();
            if (!value || Peek() != L')') return std::nullopt;
            ++at_;
            return value;
        }
        const std::size_t start = at_;
        bool point = false;
        while (at_ < text_.size() && ((text_[at_] >= L'0' && text_[at_] <= L'9') || text_[at_] == L'.')) {
            if (text_[at_] == L'.') {
                if (point) return std::nullopt;
                point = true;
            }
            ++at_;
        }
        if (at_ == start || (at_ - start == 1 && point)) return std::nullopt;
        return std::wcstod(std::wstring(text_.substr(start, at_ - start)).c_str(), nullptr);
    }

    std::wstring_view text_;
    std::size_t at_{0};
    int operators_{0};
};

}  // namespace

LocalTime LocalTime::Now() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min};
}

const SpecialConversions& SpecialConversions::Installed() {
    static const SpecialConversions installed = [] {
        SpecialConversions loaded;
        loaded.Load(ExternalLexiconProvider::DataPackRoot() / L"special-conversions" / L"rules.tsv");
        return loaded;
    }();
    return installed;
}

bool SpecialConversions::Load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    std::ostringstream text;
    text << input.rdbuf();
    Parse(text.str());
    return !Empty();
}

void SpecialConversions::Parse(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const auto fields = SplitTabs(line);
        if (fields.size() != 4 || fields[3].empty()) continue;
        const auto section = japanese::FromUtf8(fields[0]);
        const auto language = japanese::FromUtf8(fields[1]);
        auto key = japanese::FromUtf8(fields[2]);
        auto value = japanese::FromUtf8(fields[3]);
        if (language != L"en" && language != L"ja") continue;
        if (section == L"era") {
            // key: the era's name, value: the day it began (yyyy-mm-dd).
            int year = 0, month = 0, day = 0;
            if (swscanf_s(value.c_str(), L"%d-%d-%d", &year, &month, &day) == 3) {
                eras_.push_back({year * 10000 + month * 100 + day, std::move(key)});
            }
            continue;
        }
        // Japanese is typed in full-width (the romaji table writes ASCII
        // keys that way), so its symbol keys are matched in full-width.
        if (section == L"symbol" && language == L"ja") key = japanese::ToFullWidthAscii(key);
        if (section == L"symbol" && language == L"en" &&
            std::find(englishSymbolKeys_.begin(), englishSymbolKeys_.end(), key) == englishSymbolKeys_.end()) {
            englishSymbolKeys_.push_back(key);
            if (englishSymbolEnds_.find(key.back()) == std::wstring::npos) englishSymbolEnds_.push_back(key.back());
        }
        rows_[RowKey(section, language, key)].push_back(std::move(value));
    }
    std::sort(eras_.begin(), eras_.end(), [](const Era& a, const Era& b) { return a.start > b.start; });
}

const std::vector<std::wstring>* SpecialConversions::Find(std::wstring_view section, Language language,
                                                          std::wstring_view key) const {
    const auto found = rows_.find(RowKey(section, LanguageCode(language), key));
    return found == rows_.end() ? nullptr : &found->second;
}

std::wstring SpecialConversions::Name(Language language, std::wstring_view name, std::size_t index) const {
    const auto* names = Find(L"name", language, name);
    return names && index < names->size() ? (*names)[index] : std::wstring{};
}

std::vector<std::wstring> SpecialConversions::Dates(Language language, std::wstring_view word,
                                                    const LocalTime& now) const {
    const auto* rule = Find(L"word", language, word);
    if (!rule || rule->empty()) return {};
    // "day+1": the kind, and how many of it from now.
    const std::wstring& text = rule->front();
    const auto sign = text.find_first_of(L"+-");
    const std::wstring kind = text.substr(0, sign);
    const int offset = sign == std::wstring::npos ? 0 : static_cast<int>(std::wcstol(text.c_str() + sign, nullptr, 10));
    const auto* formats = Find(L"format", language, kind);
    if (!formats) return {};

    using namespace std::chrono;
    const year_month_day today{year{now.year}, month{static_cast<unsigned>(now.month)},
                               day{static_cast<unsigned>(now.day)}};
    year_month_day date = today;
    if (kind == L"day") {
        date = year_month_day{sys_days{today} + days{offset}};
    } else if (kind == L"month" || kind == L"year") {
        const auto shifted = year_month{today.year(), today.month()} +
                             (kind == L"month" ? months{offset} : months{offset * 12});
        const auto lastDay = year_month_day_last{shifted.year(), month_day_last{shifted.month()}}.day();
        date = year_month_day{shifted.year(), shifted.month(), std::min(today.day(), lastDay)};
    } else if (kind != L"time") {
        return {};
    }
    std::vector<std::wstring> out;
    for (const auto& pattern : *formats) {
        AddUnique(out, Format(language, pattern, static_cast<int>(date.year()), static_cast<int>(static_cast<unsigned>(date.month())),
                              static_cast<int>(static_cast<unsigned>(date.day())), now));
    }
    return out;
}

std::wstring SpecialConversions::Format(Language language, std::wstring_view pattern, int year, int month, int day,
                                        const LocalTime& now) const {
    using namespace std::chrono;
    const sys_days date{year_month_day{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                       std::chrono::day{static_cast<unsigned>(day)}}};
    const unsigned weekdayIndex = weekday{date}.c_encoding();  // 0 is Sunday
    const int ymd = year * 10000 + month * 100 + day;
    const auto era = std::find_if(eras_.begin(), eras_.end(), [&](const Era& e) { return e.start <= ymd; });

    std::wstring out;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] != L'{') {
            out.push_back(pattern[i++]);
            continue;
        }
        const auto close = pattern.find(L'}', i);
        if (close == std::wstring_view::npos) return {};
        const auto token = pattern.substr(i + 1, close - i - 1);
        i = close + 1;
        std::wstring value;
        const int hour12 = now.hour % 12 == 0 ? 12 : now.hour % 12;
        if (token == L"Y") value = std::to_wstring(year);
        else if (token == L"M") value = std::to_wstring(month);
        else if (token == L"MM") value = Padded(month);
        else if (token == L"D") value = std::to_wstring(day);
        else if (token == L"DD") value = Padded(day);
        else if (token == L"Month") value = Name(language, L"month", static_cast<std::size_t>(month - 1));
        else if (token == L"W") value = Name(language, L"weekday", weekdayIndex);
        else if (token == L"E" && era != eras_.end()) value = era->name;
        else if (token == L"EY" && era != eras_.end()) {
            const int eraYear = year - era->start / 10000 + 1;
            value = eraYear == 1 ? Name(language, L"era-first", 0) : std::wstring{};
            if (value.empty()) value = std::to_wstring(eraYear);
        } else if (token == L"H") value = std::to_wstring(now.hour);
        else if (token == L"HH") value = Padded(now.hour);
        else if (token == L"I") value = std::to_wstring(hour12);
        else if (token == L"N") value = std::to_wstring(now.minute);
        else if (token == L"NN") value = Padded(now.minute);
        else if (token == L"P") value = Name(language, L"ampm", now.hour < 12 ? 0 : 1);
        // A name the pack lacks leaves the format out.
        if (value.empty()) return {};
        out += value;
    }
    return out;
}

std::vector<std::wstring> SpecialConversions::Symbols(Language language, std::wstring_view key) const {
    // Japanese keys are kept in full-width, however the symbols were typed.
    const auto* symbols = language == Language::Japanese
                              ? Find(L"symbol", language, japanese::ToFullWidthAscii(key))
                              : Find(L"symbol", language, key);
    return symbols ? *symbols : std::vector<std::wstring>{};
}

std::vector<std::wstring> SpecialConversions::Emoticons(std::wstring_view reading) const {
    const auto* faces = Find(L"emoticon", Language::Japanese, reading);
    return faces ? *faces : std::vector<std::wstring>{};
}

std::vector<std::wstring> SpecialConversions::SingleKanji(std::wstring_view reading) const {
    std::vector<std::wstring> out;
    const auto* rows = Find(L"kanji", Language::Japanese, reading);
    if (!rows) return out;
    for (const auto& row : *rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            // A kanji outside the Basic Multilingual Plane is a surrogate pair.
            const std::size_t length = (row[i] >= 0xD800 && row[i] <= 0xDBFF) && i + 1 < row.size() ? 2 : 1;
            out.push_back(row.substr(i, length));
            i += length - 1;
        }
    }
    return out;
}

std::wstring SpecialConversions::Kanji(std::wstring_view digits, bool daiji) const {
    const auto language = Language::Japanese;
    const std::wstring_view prefix = daiji ? L"daiji-" : L"kanji-";
    const auto name = [&](std::wstring_view what, std::size_t index) {
        return Name(language, std::wstring(prefix) + std::wstring(what), index);
    };
    if (digits == L"0") return name(L"digits", 0);
    std::wstring out;
    const std::size_t groups = (digits.size() + 3) / 4;
    for (std::size_t group = 0; group < groups; ++group) {
        // Groups of four digits from the left; the first may be shorter.
        const std::size_t end = digits.size() - (groups - 1 - group) * 4;
        const std::size_t begin = end >= 4 ? end - 4 : 0;
        std::wstring part;
        for (std::size_t i = begin; i < end; ++i) {
            const int digit = digits[i] - L'0';
            const std::size_t place = end - 1 - i;  // 0 ones, 1 tens, 2 hundreds, 3 thousands
            if (digit == 0) continue;
            // 十, 百 and 千 stand alone for one of them (not in 大字: 壱阡).
            if (place == 0 || digit != 1 || daiji) {
                const auto text = name(L"digits", static_cast<std::size_t>(digit));
                if (text.empty()) return {};
                part += text;
            }
            if (place > 0) {
                const auto unit = name(L"units", place - 1);
                if (unit.empty()) return {};
                part += unit;
            }
        }
        if (part.empty()) continue;
        const std::size_t level = groups - 1 - group;
        if (level > 0) {
            const auto unit = name(L"groups", level - 1);
            if (unit.empty()) return {};
            part += unit;
        }
        out += part;
    }
    return out;
}

std::vector<std::wstring> SpecialConversions::Numbers(std::wstring_view text) const {
    const auto digits = AsciiDigits(text);
    if (!digits) return {};
    std::vector<std::wstring> out;
    AddUnique(out, *digits);
    AddUnique(out, japanese::ToFullWidthAscii(*digits));
    // "0120" is a code, not a number: it only changes width.
    if (digits->size() > 1 && digits->front() == L'0') return out;
    if (digits->size() >= 4) AddUnique(out, WithCommas(*digits));
    if (digits->size() <= kLongestKanjiNumber) {
        AddUnique(out, Kanji(*digits, false));
        if (digits->size() >= 2) {
            std::wstring each;
            for (const wchar_t c : *digits) each += Name(Language::Japanese, L"kanji-digits", c - L'0');
            if (each.size() >= digits->size()) AddUnique(out, std::move(each));
        }
        AddUnique(out, Kanji(*digits, true));
    }
    if (digits->size() <= 4) {
        const int value = std::stoi(*digits);
        if (value >= 1 && value <= kLargestRoman) AddUnique(out, Roman(value));
        if (value <= kLargestCircled) AddUnique(out, Circled(value));
    }
    return out;
}

bool SpecialConversions::MayEndEnglish(wchar_t c, SpecialConversionOptions options) const noexcept {
    return (options.calculator && c == L'=') ||
           (options.symbols && englishSymbolEnds_.find(c) != std::wstring::npos);
}

std::optional<SpecialConversions::Ending> SpecialConversions::EnglishEnding(std::wstring_view text,
                                                                            SpecialConversionOptions options) const {
    const auto alphanumeric = [](wchar_t c) { return std::iswalnum(c) != 0; };
    const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    // A sum: the longest arithmetic before "=" that starts clear of a word
    // or number ("I am 3 1+2=" is 1+2).
    if (options.calculator && text.ends_with(L'=')) {
        const auto arithmetic = [&](wchar_t c) {
            return digit(c) || std::wstring_view(L".+-*/() \x00D7\x00F7").find(c) != std::wstring_view::npos;
        };
        std::size_t start = text.size() - 1;
        while (start > 0 && arithmetic(text[start - 1])) --start;
        for (; start + 1 < text.size(); ++start) {
            const wchar_t first = text[start];
            if (!digit(first) && first != L'(' && first != L'-') continue;
            if (start > 0 && (alphanumeric(text[start - 1]) || text[start - 1] == L'.')) continue;
            const auto typed = text.substr(start);
            const auto sum = Calculate(typed);
            if (!sum) continue;
            const bool spaced = typed.find(L' ') != std::wstring_view::npos;
            return Ending{typed.size(), {*sum, std::wstring(typed) + (spaced ? L" " : L"") + *sum}};
        }
    }
    // A symbol's spelling, the longest that fits. One that starts with a
    // letter or digit must not be the end of a longer word or number
    // ("11/2" and "1/1/2" are not "1/2").
    if (!options.symbols) return std::nullopt;
    const std::wstring* best = nullptr;
    for (const auto& key : englishSymbolKeys_) {
        if (!text.ends_with(key) || (best && best->size() >= key.size())) continue;
        const std::size_t start = text.size() - key.size();
        if (start > 0 && alphanumeric(key.front())) {
            const wchar_t before = text[start - 1];
            if (alphanumeric(before) || std::wstring_view(L"/.,").find(before) != std::wstring_view::npos) continue;
        }
        best = &key;
    }
    if (!best) return std::nullopt;
    return Ending{best->size(), Symbols(Language::English, *best)};
}

std::optional<std::wstring> SpecialConversions::Calculate(std::wstring_view expression) {
    std::wstring text;
    for (const wchar_t c : expression) {
        wchar_t ascii = c >= L'\xFF01' && c <= L'\xFF5E' ? static_cast<wchar_t>(c - 0xFEE0) : c;
        switch (ascii) {
        case L'\x30FC':  // ー: Japanese input types "-" as the long-vowel mark
        case L'\x2212':  // −
        case L'\x2015':
            ascii = L'-';
            break;
        case L'\x00D7':  // ×
            ascii = L'*';
            break;
        case L'\x00F7':  // ÷
        case L'\x30FB':  // ・: and "/" as the middle dot
            ascii = L'/';
            break;
        case L'\x3000':
            ascii = L' ';
            break;
        default:
            break;
        }
        text.push_back(ascii);
    }
    if (text.size() < 2 || text.back() != L'=') return std::nullopt;
    text.pop_back();
    const auto value = Arithmetic(text).Evaluate();
    if (!value || !std::isfinite(*value)) return std::nullopt;
    double result = *value;
    if (result == 0.0) result = 0.0;  // no "-0"
    wchar_t buffer[64]{};
    if (std::abs(result) < 1e15 && result == std::floor(result)) {
        swprintf_s(buffer, L"%.0f", result);
    } else {
        swprintf_s(buffer, L"%.10g", result);
    }
    return std::wstring(buffer);
}

}  // namespace tekito
