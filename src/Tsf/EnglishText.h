#pragma once

#include "Core/InputStateMachine.h"

#include <cwchar>
#include <cwctype>
#include <string_view>

namespace tekito::tsf {

// How English typing reads the characters around a word. Shared by the text
// service and the testbed so both type alike.

// Punctuation that ends a word: the sentence enders and the clause marks.
inline bool TryClassifyPunctuation(wchar_t ch, PunctuationRole& role) {
    switch (ch) {
    case L'.':
    case L'!':
    case L'?':
        role = PunctuationRole::SentenceTerminal;
        return true;
    case L',':
    case L':':
    case L';':
        role = PunctuationRole::ClauseSeparator;
        return true;
    default:
        return false;
    }
}

// Letters and the apostrophe inside contractions ("don't", "it's").
inline bool IsWordCharacter(wchar_t ch) {
    return std::iswalpha(ch) != 0 || ch == L'\'' || ch == L'\u2019';
}

// A word may begin after whitespace, an opening bracket or quote, a dash, or
// Japanese text.
// Right after anything else -- a letter or digit we did not compose, or the
// '/', '.', '@', ':', '_' of a URL, address, path or identifier -- the typing
// is part of a token that must be left exactly as typed.
inline bool CanStartWordAfter(std::wstring_view preceding) {
    if (preceding.empty()) return true;
    const wchar_t ch = preceding.back();
    if (std::iswspace(ch)) return true;
    // Japanese text puts English words right after kana, kanji or
    // full-width marks, with no space.
    if (ch >= L'\u3000') return true;
    switch (ch) {
    case L'(': case L'[': case L'{': case L'<':
    case L'"': case L'\'': case L'\u201C': case L'\u2018': case L'\u00AB':
    case L'-': case L'\u2013': case L'\u2014':
    case L'*':
        return true;
    default:
        return false;
    }
}

// "word--" and "word --": the second hyphen makes an em dash of the first.
inline bool MakesEmDash(std::wstring_view preceding) {
    return preceding.size() >= 2 && preceding.back() == L'-' &&
           (std::iswalnum(preceding[preceding.size() - 2]) || preceding[preceding.size() - 2] == L' ');
}

// The curly quote for " or ': opening after a space, a bracket, another
// opening quote or a dash; closing (and the apostrophe) after anything else.
inline wchar_t SmartQuote(wchar_t character, std::wstring_view preceding) {
    const wchar_t before = preceding.empty() ? L'\n' : preceding.back();
    const bool opening = std::iswspace(before) || std::wcschr(L"([{<\u201C\u2018\u2014\u2013-", before) != nullptr ||
                         (before >= L'\u3000' && before != L'\u2019' && before != L'\u201D');
    return character == L'"' ? (opening ? L'\u201C' : L'\u201D') : (opening ? L'\u2018' : L'\u2019');
}

}  // namespace tekito::tsf
