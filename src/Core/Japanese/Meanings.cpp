#include "Core/Japanese/Meanings.h"

#include "Core/DataPackPath.h"
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
    slang_.Open(FindDataPack(dataRoot, L"japanese-slang") / L"meanings.tsv");
    wiktionary_.Open(FindDataPack(dataRoot, L"japanese-wiktionary") / L"meanings.tsv");
    wordnet_.Open(FindDataPack(dataRoot, L"japanese-wordnet") / L"meanings.tsv");
    english_.Open(FindDataPack(dataRoot, L"dictionary-display") / L"entries.tsv");
    englishInJapanese_.Open(FindDataPack(dataRoot, L"english-meanings-ja") / L"meanings.tsv");
    japaneseInEnglish_.Open(FindDataPack(dataRoot, L"japanese-meanings-en") / L"meanings.tsv");
    forms_.Open(FindDataPack(dataRoot, L"english-forms") / L"forms.tsv");
    return IsOpen();
}

std::optional<Meaning> MeaningDictionary::Lookup(std::wstring_view text, std::wstring_view reading,
                                                 MeaningLanguage language) const {
    if (text.empty()) return std::nullopt;
    try {
        if (IsLatinWord(text)) {
            if (text.size() > kMaxEnglishWord) return Latin(text, language);
            std::wstring lower(text);
            for (wchar_t& c : lower) c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            const auto forms = FormsOf(lower);
            if (auto meaning = Latin(text, language)) {
                // A word of its own that is also an irregular form (gone,
                // best): its meaning, with the form too. Regular endings are
                // not looked at here (evening is no form of even).
                if (forms_.IsOpen()) {
                    forms_.ForEachRow(ToUtf8(lower), [&](const std::vector<std::string_view>& fields) {
                        if (fields.size() < 3 || fields[1].empty()) return true;
                        meaning->base = FromUtf8(fields[1]);
                        meaning->form = FromUtf8(fields[2]);
                        return false;
                    });
                }
                return meaning;
            }
            // Another form of a word (went, called, players): that word's
            // meaning, with the form.
            for (const auto& [base, form] : forms) {
                if (auto meaning = Latin(base, language)) {
                    meaning->headword = std::wstring(text);
                    meaning->base = base;
                    meaning->form = form;
                    return meaning;
                }
            }
            return std::nullopt;
        }
        const std::wstring hiragana = ToHiragana(reading);
        if (language == MeaningLanguage::English) {
            if (auto meaning = Japanese(japaneseInEnglish_, text, hiragana)) return meaning;
        }
        for (const SortedTsv* source : {&slang_, &wiktionary_, &wordnet_}) {
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

std::optional<Meaning> MeaningDictionary::Latin(std::wstring_view word, MeaningLanguage language) const {
    if (language == MeaningLanguage::Japanese && word.size() <= kMaxEnglishWord) {
        std::wstring lower(word);
        for (wchar_t& c : lower) c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        if (auto meaning = Japanese(englishInJapanese_, lower, {})) return meaning;
    }
    return English(word);
}

std::vector<std::string> MeaningDictionary::PartsOfSpeech(std::wstring_view word) const {
    // english-forms: the word's row with no base, "parts:noun verb".
    std::vector<std::string> parts;
    if (!forms_.IsOpen()) return parts;
    forms_.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
        if (fields.size() < 3 || !fields[1].empty() || !fields[2].starts_with("parts:")) return true;
        std::string_view names = fields[2].substr(6);
        while (!names.empty()) {
            const std::size_t space = names.find(' ');
            parts.emplace_back(names.substr(0, space));
            if (space == std::string_view::npos) break;
            names.remove_prefix(space + 1);
        }
        return false;
    });
    return parts;
}

std::vector<std::pair<std::wstring, std::wstring>> MeaningDictionary::FormsOf(std::wstring_view lower) const {
    std::vector<std::pair<std::wstring, std::wstring>> forms;
    if (forms_.IsOpen()) {
        forms_.ForEachRow(ToUtf8(lower), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 3 && !fields[1].empty()) forms.emplace_back(FromUtf8(fields[1]), FromUtf8(fields[2]));
            return true;
        });
    }
    if (!forms.empty() || lower.size() < 4) return forms;
    const std::wstring word(lower);
    const auto ends = [&](std::wstring_view end) { return word.size() > end.size() + 1 && word.ends_with(end); };
    const auto cut = [&](std::size_t n) { return word.substr(0, word.size() - n); };
    // A doubled last consonant (running, bigger, stopped): one of it.
    const auto undoubled = [&](std::size_t n) {
        const std::wstring stem = cut(n);
        return stem.size() >= 3 && stem.back() == stem[stem.size() - 2] ? stem.substr(0, stem.size() - 1) : std::wstring{};
    };
    // The word's parts of speech; one the dictionary does not have is taken
    // as any (the Japanese meanings name none).
    const auto is = [&](const std::wstring& base, std::initializer_list<std::string_view> wanted) {
        const auto parts = PartsOfSpeech(base);
        if (parts.empty()) return true;
        return std::any_of(parts.begin(), parts.end(), [&](const std::string& part) {
            return std::find(wanted.begin(), wanted.end(), part) != wanted.end();
        });
    };
    const auto add = [&](const std::wstring& base, std::wstring_view form) {
        if (base.size() < 2) return;
        if (std::any_of(forms.begin(), forms.end(), [&](const auto& f) { return f.first == base; })) return;
        forms.emplace_back(base, std::wstring(form));
    };
    if (ends(L"s") && !ends(L"ss")) {
        std::vector<std::wstring> bases;
        if (ends(L"ies")) bases.push_back(cut(3) + L"y");
        if (ends(L"es")) bases.push_back(cut(2));
        bases.push_back(cut(1));
        for (const auto& base : bases) {
            const bool noun = is(base, {"noun"});
            const bool verb = is(base, {"verb"});
            if (!noun && !verb) continue;
            const auto parts = PartsOfSpeech(base);
            const bool both = parts.empty() || (std::find(parts.begin(), parts.end(), "noun") != parts.end() &&
                                                std::find(parts.begin(), parts.end(), "verb") != parts.end());
            add(base, both ? L"plural+third-person" : noun ? L"plural" : L"third-person");
        }
    }
    if (ends(L"ed")) {
        for (const auto& base : {ends(L"ied") ? cut(3) + L"y" : std::wstring{}, cut(2), cut(1), undoubled(2)}) {
            if (!base.empty() && is(base, {"verb"})) add(base, L"past+past-participle");
        }
    }
    if (ends(L"ing")) {
        for (const auto& base : {cut(3), cut(3) + L"e", undoubled(3)}) {
            if (!base.empty() && is(base, {"verb"})) add(base, L"present-participle");
        }
    }
    for (const auto& [end, form] : {std::pair{std::wstring_view(L"er"), std::wstring_view(L"comparative")},
                                    std::pair{std::wstring_view(L"est"), std::wstring_view(L"superlative")}}) {
        if (!ends(end)) continue;
        const std::size_t n = end.size();
        for (const auto& base : {ends(std::wstring(L"i") + std::wstring(end)) ? cut(n + 1) + L"y" : std::wstring{}, cut(n),
                                 cut(n - 1), undoubled(n)}) {
            if (!base.empty() && is(base, {"adj", "adv"})) add(base, form);
        }
    }
    return forms;
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
