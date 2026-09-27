#pragma once

#include <string>
#include <string_view>

namespace tekito::japanese {

// Character-level conversions between the ways Japanese text is written.
// These follow Unicode, not vocabulary: every function maps characters one
// by one (or to a base character plus a sound mark) and leaves anything it
// has no mapping for unchanged.

[[nodiscard]] std::wstring ToKatakana(std::wstring_view text);
[[nodiscard]] std::wstring ToHiragana(std::wstring_view text);
// Katakana, the Japanese punctuation and full-width ASCII to their
// half-width forms; hiragana is first turned into katakana.
[[nodiscard]] std::wstring ToHalfWidthKatakana(std::wstring_view text);
// ASCII (including the space) to full-width, and back.
[[nodiscard]] std::wstring ToFullWidthAscii(std::wstring_view text);
[[nodiscard]] std::wstring ToHalfWidthAscii(std::wstring_view text);

}  // namespace tekito::japanese
