#pragma once

#include "Core/Candidate.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace tekito {

class AutoApplyPolicy final {
public:
    [[nodiscard]] std::optional<std::size_t> SelectForBoundary(
        std::wstring_view rawText, std::span<const Candidate> candidates,
        std::optional<std::size_t> explicitSelection = std::nullopt) const noexcept;

private:
    [[nodiscard]] bool IsSafeAutomaticCandidate(std::wstring_view rawText,
                                                const Candidate& candidate) const noexcept;
};

}  // namespace tekito
