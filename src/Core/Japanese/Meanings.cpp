#include "Core/Japanese/Meanings.h"

#include "Core/Japanese/KanaText.h"

#include <algorithm>
#include <array>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace tekito::japanese {
namespace {

constexpr std::size_t kMaxSenses = 4;
constexpr std::size_t kMaxEnglishWord = 48;
// Endings that make a dictionary form from an inflected stem: 会っ(た) ->
// 会う, 食べ(ました) -> 食べる, 高(かった) -> 高い.
constexpr std::array<wchar_t, 11> kEndings{L'う', L'く', L'ぐ', L'す', L'つ', L'ぬ', L'ぶ', L'む', L'る', L'い', L'だ'};

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(std::max(length, 0)), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr,
                            nullptr);
    }
    return bytes;
}

std::wstring FromUtf8(std::string_view bytes) {
    if (bytes.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    }
    return text;
}

bool IsHiragana(wchar_t c) noexcept { return c >= 0x3041 && c <= 0x309F; }

bool IsLatinWord(std::wstring_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'\'' || c == L'-' || c == L' ';
    });
}

}  // namespace

bool SortedTsv::Open(const std::filesystem::path& path) noexcept {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    return file_.Open(path);
}

std::size_t SortedTsv::LineStart(std::size_t at) const noexcept {
    // The first line that starts at or after `at`.
    if (at == 0) return 0;
    const auto* data = file_.Data();
    const std::size_t size = file_.Size();
    std::size_t i = at - 1;
    while (i < size && data[i] != '\n') ++i;
    return std::min(i + 1, size);
}

std::string_view SortedTsv::KeyAt(std::size_t lineStart) const noexcept {
    const auto* data = reinterpret_cast<const char*>(file_.Data());
    const std::size_t size = file_.Size();
    std::size_t end = lineStart;
    while (end < size && data[end] != '\t' && data[end] != '\n' && data[end] != '\r') ++end;
    return {data + lineStart, end - lineStart};
}

void SortedTsv::ForEachRow(std::string_view key,
                           const std::function<bool(const std::vector<std::string_view>& fields)>& row) const {
    if (!IsOpen() || key.empty()) return;
    const std::size_t size = file_.Size();
    std::size_t lo = 0, hi = size;
    while (lo < hi) {
        const std::size_t middle = lo + (hi - lo) / 2;
        const std::size_t start = LineStart(middle);
        if (start >= size || KeyAt(start) >= key) hi = middle; else lo = middle + 1;
    }
    const auto* data = reinterpret_cast<const char*>(file_.Data());
    std::vector<std::string_view> fields;
    for (std::size_t start = LineStart(lo); start < size;) {
        if (KeyAt(start) != key) return;
        std::size_t end = start;
        while (end < size && data[end] != '\n') ++end;
        std::string_view line(data + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fields.clear();
        for (std::size_t from = 0;;) {
            const std::size_t tab = line.find('\t', from);
            fields.push_back(line.substr(from, tab == std::string_view::npos ? std::string_view::npos : tab - from));
            if (tab == std::string_view::npos) break;
            from = tab + 1;
        }
        if (!row(fields)) return;
        start = end + 1;
    }
}

bool MeaningDictionary::Open(const std::filesystem::path& dataRoot) noexcept {
    wiktionary_.Open(dataRoot / L"japanese-wiktionary" / L"meanings.tsv");
    wordnet_.Open(dataRoot / L"japanese-wordnet" / L"meanings.tsv");
    english_.Open(dataRoot / L"dictionary-display" / L"entries.tsv");
    return IsOpen();
}

std::optional<Meaning> MeaningDictionary::Lookup(std::wstring_view text, std::wstring_view reading) const {
    if (text.empty()) return std::nullopt;
    try {
        if (IsLatinWord(text)) return English(text);
        const std::wstring hiragana = ToHiragana(reading);
        for (const SortedTsv* source : {&wiktionary_, &wordnet_}) {
            if (auto meaning = Japanese(*source, text, hiragana)) return meaning;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<Meaning> MeaningDictionary::Japanese(const SortedTsv& source, std::wstring_view text,
                                                   std::wstring_view reading) const {
    if (!source.IsOpen()) return std::nullopt;
    // The senses of `word`: with `strict`, only from a row for exactly
    // `wordReading`; otherwise from that row if there is one, else the row
    // that names no reading, else the first.
    const auto find = [&](std::wstring_view word, std::wstring_view wordReading,
                          bool strict) -> std::optional<Meaning> {
        if (strict && wordReading.empty()) return std::nullopt;
        const std::string wanted = ToUtf8(wordReading);
        std::vector<std::string_view> chosen;
        int rank = 3;
        source.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() < 3) return true;
            const int r = !wanted.empty() && fields[1] == wanted ? 0 : fields[1].empty() ? 1 : 2;
            if ((strict && r != 0) || r >= rank) return true;
            rank = r;
            chosen = fields;
            return r != 0;
        });
        if (chosen.empty()) return std::nullopt;
        Meaning meaning{std::wstring(word), {}};
        for (std::size_t i = 2; i < chosen.size() && meaning.senses.size() < kMaxSenses; ++i) {
            if (!chosen[i].empty()) meaning.senses.push_back(FromUtf8(chosen[i]));
        }
        if (meaning.senses.empty()) return std::nullopt;
        return meaning;
    };

    // The candidate as it is, then shorter stems of it with the endings of
    // dictionary forms: 会った -> 会っ, 会 (+う) ; 私の -> 私. Only while a
    // stem keeps a character that is not hiragana: kana alone is too
    // ambiguous to cut (はしった is not はし).
    for (const bool strict : {true, false}) {
        if (auto meaning = find(text, reading, strict)) return meaning;
        std::wstring stem(text);
        std::wstring stemReading(reading);
        while (stem.size() > 1 && IsHiragana(stem.back())) {
            const wchar_t dropped = stem.back();
            stem.pop_back();
            if (!stemReading.empty() && stemReading.back() == dropped) stemReading.pop_back(); else stemReading.clear();
            if (std::all_of(stem.begin(), stem.end(), IsHiragana)) break;
            for (const wchar_t ending : kEndings) {
                const std::wstring form = stem + ending;
                const std::wstring formReading = stemReading.empty() ? std::wstring{} : stemReading + ending;
                if (auto meaning = find(form, formReading, strict)) return meaning;
            }
            if (auto meaning = find(stem, stemReading, strict)) return meaning;
        }
    }
    return std::nullopt;
}

std::optional<Meaning> MeaningDictionary::English(std::wstring_view word) const {
    if (!english_.IsOpen() || word.size() > kMaxEnglishWord) return std::nullopt;
    std::string key = "external:";
    for (const wchar_t c : word) key.push_back(static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
    std::optional<Meaning> meaning;
    // entries.tsv: key, word, pronunciation, part of speech, senses joined
    // with " / ", example.
    english_.ForEachRow(key, [&](const std::vector<std::string_view>& fields) {
        if (fields.size() < 5 || fields[4].empty()) return true;
        meaning = Meaning{FromUtf8(fields[1]), {}};
        std::string_view senses = fields[4];
        while (!senses.empty() && meaning->senses.size() < kMaxSenses) {
            const std::size_t split = senses.find(" / ");
            meaning->senses.push_back(FromUtf8(senses.substr(0, split)));
            if (split == std::string_view::npos) break;
            senses.remove_prefix(split + 3);
        }
        return false;
    });
    return meaning;
}

}  // namespace tekito::japanese
