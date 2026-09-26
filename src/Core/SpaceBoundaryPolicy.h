#pragma once

#include "Core/Candidate.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace tekito {

class SpaceBoundaryPolicy final {
public:
    [[nodiscard]] std::optional<std::size_t> SelectCorrection(
        std::wstring_view rawText, std::span<const Candidate> candidates) const noexcept;

private:
    [[nodiscard]] bool IsAcceptableCorrection(std::wstring_view rawText,
                                               const Candidate& candidate) const noexcept;
};

}  // namespace tekito
