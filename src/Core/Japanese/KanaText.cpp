#include "Core/Japanese/KanaText.h"

#include <iterator>

namespace tekito::japanese {
namespace {

constexpr wchar_t kFirstSmallHiragana = L'ぁ';  // U+3041
constexpr wchar_t kLastHiragana = L'ゖ';        // U+3096
constexpr wchar_t kFirstSmallKatakana = L'ァ';  // U+30A1
constexpr wchar_t kLastKatakana = L'ヶ';        // U+30F6
constexpr wchar_t kKanaOffset = kFirstSmallKatakana - kFirstSmallHiragana;

// Half-width forms of ァ (U+30A1) through ヶ (U+30F6), in code point order.
// Voiced and semi-voiced kana become the base kana plus ﾞ or ﾟ; the few
// with no half-width form use the nearest one (ヮ -> ﾜ, ヰ -> ｲ, ヵ -> ｶ).
constexpr const wchar_t* kHalfWidthKatakana[] = {
    L"ｧ", L"ｱ", L"ｨ", L"ｲ", L"ｩ", L"ｳ", L"ｪ", L"ｴ", L"ｫ", L"ｵ",
    L"ｶ", L"ｶﾞ", L"ｷ", L"ｷﾞ", L"ｸ", L"ｸﾞ", L"ｹ", L"ｹﾞ", L"ｺ", L"ｺﾞ",
    L"ｻ", L"ｻﾞ", L"ｼ", L"ｼﾞ", L"ｽ", L"ｽﾞ", L"ｾ", L"ｾﾞ", L"ｿ", L"ｿﾞ",
    L"ﾀ", L"ﾀﾞ", L"ﾁ", L"ﾁﾞ", L"ｯ", L"ﾂ", L"ﾂﾞ", L"ﾃ", L"ﾃﾞ", L"ﾄ", L"ﾄﾞ",
    L"ﾅ", L"ﾆ", L"ﾇ", L"ﾈ", L"ﾉ",
    L"ﾊ", L"ﾊﾞ", L"ﾊﾟ", L"ﾋ", L"ﾋﾞ", L"ﾋﾟ", L"ﾌ", L"ﾌﾞ", L"ﾌﾟ",
    L"ﾍ", L"ﾍﾞ", L"ﾍﾟ", L"ﾎ", L"ﾎﾞ", L"ﾎﾟ",
    L"ﾏ", L"ﾐ", L"ﾑ", L"ﾒ", L"ﾓ",
    L"ｬ", L"ﾔ", L"ｭ", L"ﾕ", L"ｮ", L"ﾖ",
    L"ﾗ", L"ﾘ", L"ﾙ", L"ﾚ", L"ﾛ",
    L"ﾜ", L"ﾜ", L"ｲ", L"ｴ", L"ｦ", L"ﾝ", L"ｳﾞ", L"ｶ", L"ｹ",
};
static_assert(std::size(kHalfWidthKatakana) == kLastKatakana - kFirstSmallKatakana + 1);

const wchar_t* HalfWidthPunctuation(wchar_t ch) {
    switch (ch) {
    case L'。': return L"｡";
    case L'「': return L"｢";
    case L'」': return L"｣";
    case L'、': return L"､";
    case L'・': return L"･";
    case L'ー': return L"ｰ";
    case L'゛': return L"ﾞ";
    case L'゜': return L"ﾟ";
    default: return nullptr;
    }
}

}  // namespace

std::wstring ToKatakana(std::wstring_view text) {
    std::wstring out(text);
    for (auto& ch : out) {
        if (ch >= kFirstSmallHiragana && ch <= kLastHiragana) {
            ch = static_cast<wchar_t>(ch + kKanaOffset);
        } else if (ch == L'ゝ' || ch == L'ゞ') {
            ch = static_cast<wchar_t>(ch + kKanaOffset);
        }
    }
    return out;
}

std::wstring ToHiragana(std::wstring_view text) {
    std::wstring out(text);
    for (auto& ch : out) {
        if (ch >= kFirstSmallKatakana && ch <= kLastKatakana) {
            ch = static_cast<wchar_t>(ch - kKanaOffset);
        } else if (ch == L'ヽ' || ch == L'ヾ') {
            ch = static_cast<wchar_t>(ch - kKanaOffset);
        }
    }
    return out;
}

std::wstring ToHalfWidthKatakana(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size() * 2);
    for (const wchar_t original : ToKatakana(text)) {
        if (original >= kFirstSmallKatakana && original <= kLastKatakana) {
            out += kHalfWidthKatakana[original - kFirstSmallKatakana];
        } else if (const wchar_t* half = HalfWidthPunctuation(original)) {
            out += half;
        } else {
            out += ToHalfWidthAscii(std::wstring_view(&original, 1));
        }
    }
    return out;
}

std::wstring ToFullWidthAscii(std::wstring_view text) {
    std::wstring out(text);
    for (auto& ch : out) {
        if (ch == L' ') {
            ch = L'　';
        } else if (ch >= 0x21 && ch <= 0x7E) {
            ch = static_cast<wchar_t>(ch + 0xFEE0);
        }
    }
    return out;
}

std::wstring ToHalfWidthAscii(std::wstring_view text) {
    std::wstring out(text);
    for (auto& ch : out) {
        if (ch == L'　') {
            ch = L' ';
        } else if (ch >= 0xFF01 && ch <= 0xFF5E) {
            ch = static_cast<wchar_t>(ch - 0xFEE0);
        }
    }
    return out;
}

}  // namespace tekito::japanese
