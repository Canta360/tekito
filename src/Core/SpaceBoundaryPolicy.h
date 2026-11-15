#pragma once

#include "Core/Candidate.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace tekito {

// Whether a correction is what was likely meant: a known misspelling or
// the user's word, or a word the typed keys read as with slips a hand makes
// (IsPlausibleSlip), not a word the dictionary has near one it lacks.
[[nodiscard]] bool IsLikelyMeant(std::wstring_view rawText, const Candidate& candidate) noexcept;

class SpaceBoundaryPolicy final {
public:
    [[nodiscard]] std::optional<std::size_t> SelectCorrection(
        std::wstring_view rawText, std::span<const Candidate> candidates) const noexcept;

private:
    [[nodiscard]] bool IsAcceptableCorrection(std::wstring_view rawText,
                                               const Candidate& candidate) const noexcept;
};

}  // namespace tekito
