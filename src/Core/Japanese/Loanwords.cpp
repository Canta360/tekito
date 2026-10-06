#include "Core/Japanese/Loanwords.h"

#include "Core/Japanese/KanaText.h"

#include <cwctype>

namespace tekito::japanese {
namespace {

// Hiragana by the vowel they end in, for the long-vowel mark.
constexpr std::wstring_view kVowelA = L"あかさたなはまやらわがざだばぱぁゃゎ";
constexpr std::wstring_view kVowelI = L"いきしちにひみりぎじぢびぴぃ";
constexpr std::wstring_view kVowelU = L"うくすつぬふむゆるぐずづぶぷぅゅゔ";
constexpr std::wstring_view kVowelE = L"えけせてねへめれげぜでべぺぇ";
constexpr std::wstring_view kVowelO = L"おこそとのほもよろをごぞどぼぽぉょ";

wchar_t VowelOf(wchar_t kana) {
    if (kVowelA.find(kana) != std::wstring_view::npos) return L'あ';
    if (kVowelI.find(kana) != std::wstring_view::npos) return L'い';
    if (kVowelU.find(kana) != std::wstring_view::npos) return L'う';
    if (kVowelE.find(kana) != std::wstring_view::npos) return L'え';
    if (kVowelO.find(kana) != std::wstring_view::npos) return L'お';
    return 0;
}

void ReplaceAll(std::wstring& text, std::wstring_view from, std::wstring_view to) {
    for (std::size_t at = text.find(from); at != std::wstring::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

}  // namespace

std::wstring LoanwordKey(std::wstring_view kana) {
    std::wstring text = ToHiragana(kana);
    ReplaceAll(text, L"てぃ", L"ち");
    ReplaceAll(text, L"でぃ", L"じ");
    ReplaceAll(text, L"ぢ", L"じ");
    ReplaceAll(text, L"づ", L"ず");
    ReplaceAll(text, L"ゔ", L"ぶ");
    std::wstring key;
    for (const wchar_t c : text) {
        if (c == L'ー') {
            if (const wchar_t vowel = key.empty() ? 0 : VowelOf(key.back())) key += vowel;
            continue;
        }
        key += c;
    }
    for (auto& c : key) {
        switch (c) {
            case L'ぁ': c = L'あ'; break;
            case L'ぃ': c = L'い'; break;
            case L'ぅ': c = L'う'; break;
            case L'ぇ': c = L'え'; break;
            case L'ぉ': c = L'お'; break;
            default: break;
        }
    }
    return key;
}

bool Loanwords::Open(const std::filesystem::path& packDirectory) noexcept {
    return words_.Open(packDirectory / L"words.tsv");
}

bool Loanwords::OpenEnglish(const std::filesystem::path& packDirectory) noexcept {
    const bool names = names_.Open(packDirectory / L"words.tsv");
    // Counts came later: a pack without them still opens.
    (void)latin_.Open(packDirectory / L"latin.tsv");
    return english_.Open(packDirectory / L"english.tsv") && names;
}

std::vector<std::wstring> Loanwords::Words(std::wstring_view katakana, std::size_t limit) const {
    std::vector<std::wstring> words;
    if (!IsOpen() || katakana.empty() || limit == 0) return words;
    const auto add = [&](std::wstring word) {
        // One spelling per word: Zoom and zoom are the same choice.
        const auto lower = [](std::wstring text) {
            for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
            return text;
        };
        for (const auto& seen : words) {
            if (lower(seen) == lower(word)) return;
        }
        if (words.size() < limit) words.push_back(std::move(word));
    };
    try {
        const auto key = ToUtf8(LoanwordKey(katakana));
        // japanese-english rows: key, English, katakana, score, best first.
        // Those written with this very katakana come first.
        std::vector<std::wstring> others;
        const std::wstring written(katakana);
        names_.ForEachRow(key, [&](const std::vector<std::string_view>& fields) {
            if (fields.size() < 3 || fields[1].empty()) return true;
            if (FromUtf8(fields[2]) == written) {
                add(FromUtf8(fields[1]));
            } else {
                others.push_back(FromUtf8(fields[1]));
            }
            return true;
        });
        // Spellings that only sound alike (Google Play for グーグル) only
        // when this katakana has none of its own.
        if (words.empty()) {
            for (auto& word : others) add(std::move(word));
        }
        // japanese-loanwords rows: key, word, how common it is.
        words_.ForEachRow(key, [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 2 && !fields[1].empty()) add(FromUtf8(fields[1]));
            return words.size() < limit;
        });
    } catch (...) {
        words.clear();
    }
    return words;
}

std::optional<double> Loanwords::InJapaneseText(std::wstring_view word) const {
    if (!latin_.IsOpen() || word.empty()) return std::nullopt;
    std::optional<double> found;
    try {
        // Rows: word in lower case, log10 per million sentences.
        latin_.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 2) found = std::stod(std::string(fields[1]));
            return false;
        });
    } catch (...) {
        found.reset();
    }
    return found;
}

std::vector<Loanwords::Spelling> Loanwords::ForEnglish(std::wstring_view english, std::size_t limit) const {
    std::vector<Spelling> spellings;
    if (!english_.IsOpen() || english.empty() || limit == 0) return spellings;
    std::wstring key;
    for (const wchar_t c : english) {
        if (c != L' ') key += static_cast<wchar_t>(std::towlower(c));
    }
    try {
        // Rows: English in lower case without spaces, English, katakana, score.
        english_.ForEachRow(ToUtf8(key), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 4 && !fields[1].empty()) {
                spellings.push_back({FromUtf8(fields[1]), FromUtf8(fields[2]), std::stod(std::string(fields[3]))});
            }
            return spellings.size() < limit;
        });
    } catch (...) {
        spellings.clear();
    }
    return spellings;
}

}  // namespace tekito::japanese
