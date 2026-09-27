#include "Core/Japanese/Loanwords.h"

#include "Core/Japanese/KanaText.h"

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

std::vector<std::wstring> Loanwords::Words(std::wstring_view katakana, std::size_t limit) const {
    std::vector<std::wstring> words;
    if (!IsOpen() || katakana.empty() || limit == 0) return words;
    try {
        // Rows: key, word, how common it is; a key's rows most common first.
        words_.ForEachRow(ToUtf8(LoanwordKey(katakana)), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 2 && !fields[1].empty()) words.push_back(FromUtf8(fields[1]));
            return words.size() < limit;
        });
    } catch (...) {
        words.clear();
    }
    return words;
}

}  // namespace tekito::japanese
