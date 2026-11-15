#pragma once

#include <cstddef>
#include <string_view>

namespace tekito {

struct TypoEvidence {
    std::size_t distance{0};
    bool repeatedKey{false};
    bool transposition{false};
    bool missingCharacter{false};
    bool extraCharacter{false};
    bool qwertyNeighbor{false};
};

// Whether two letters sit next to each other on a QWERTY keyboard (same
// row, or the row above or below).
[[nodiscard]] bool AreQwertyNeighbors(wchar_t left, wchar_t right) noexcept;

// Whether `typed` reads as `word` typed with slips a hand makes: a key next
// to the right one (or a vowel for another, as English is misspelled), two
// keys swapped, a key left out, or one added next to
// its neighbors or doubled; at most one for a word of up to four letters,
// two for a longer one. A letter for one far from it on the keyboard
// (kaggle for gaggle, pixiv for pixie) is no slip: a word the dictionary
// lacks, typed as meant.
[[nodiscard]] bool IsPlausibleSlip(std::wstring_view typed, std::wstring_view word) noexcept;

// General typo evidence only. It does not contain misspelling->correction
// rows and does not decide ranking or automatic application.
class TypoModel final {
public:
    [[nodiscard]] TypoEvidence Compare(std::wstring_view input,
                                       std::wstring_view target) const noexcept;

    // Edit distance with insertions/deletions costed above substitutions
    // (matching classical weighted spelling-correction models, where a
    // dropped/added character is a less common typing error than fat-
    // fingering an adjacent key), so ranking can weigh a candidate's error
    // plausibility as a continuous cost instead of an integer distance that
    // makes every distance-1 candidate beat every distance-2 one outright.
    [[nodiscard]] double WeightedCost(std::wstring_view input,
                                      std::wstring_view target) const noexcept;
};

}  // namespace tekito
