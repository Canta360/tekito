#include "Core/Japanese/Meanings.h"

#include "Core/Japanese/KanaText.h"

#include <algorithm>
#include <array>

namespace tekito::japanese {
namespace {

constexpr std::size_t kMaxSenses = 4;
constexpr std::size_t kMaxEnglishWord = 48;
// Endings that make a dictionary form from an inflected stem: 会っ(た) ->
// 会う, 食べ(ました) -> 食べる, 高(かった) -> 高い.
constexpr std::array<wchar_t, 11> kEndings{L'う', L'く', L'ぐ', L'す', L'つ', L'ぬ', L'ぶ', L'む', L'る', L'い', L'だ'};

bool IsHiragana(wchar_t c) noexcept { return c >= 0x3041 && c <= 0x309F; }

bool IsLatinWord(std::wstring_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'\'' || c == L'-' || c == L' ';
    });
}

}  // namespace

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
