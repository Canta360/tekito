#pragma once

#include "Core/BuiltinLexicon.h"

#include <cstdint>

namespace tekito {

class PolicyEngine final {
public:
    [[nodiscard]] static bool IsTekitoOwnedExpression(SemanticLabel label) noexcept;
    [[nodiscard]] std::uint32_t RawFlags(const LexiconEntry& entry) const noexcept;
    [[nodiscard]] std::uint32_t CandidateFlags(const LexiconEntry& entry) const noexcept;
};

}  // namespace tekito
