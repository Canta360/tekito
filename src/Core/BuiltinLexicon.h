#pragma once

#include "Core/Candidate.h"

#include <span>
#include <string_view>

namespace tekito {

struct LexiconEntry {
    std::wstring_view raw;
    std::wstring_view candidate;
    SemanticLabel rawLabel;
    SemanticLabel candidateLabel;
    bool originalFirst;
    bool spellcheckWord;
    std::uint32_t rawPolicyFlags{CandidatePolicyNone};
    std::uint32_t candidatePolicyFlags{CandidatePolicyNone};
    std::uint32_t sourceFlags{CandidateSourceEmbeddedLexicon};
    bool caseSensitive{false};
    std::uint8_t socialRange{SocialRangeUnspecified};
};

// BuiltinLexiconEntry remains as a source-compatible name for the fallback
// table; provider boundaries use the original LexiconEntry ABI name.
using BuiltinLexiconEntry = LexiconEntry;

[[nodiscard]] std::span<const LexiconEntry> BuiltinLexicon() noexcept;

}  // namespace tekito
