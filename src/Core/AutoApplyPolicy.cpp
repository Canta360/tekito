#include "Core/AutoApplyPolicy.h"

#include <algorithm>

namespace tekito {

std::optional<std::size_t> AutoApplyPolicy::SelectForBoundary(
    std::wstring_view rawText, std::span<const Candidate> candidates,
    std::optional<std::size_t> explicitSelection) const noexcept {
    if (explicitSelection && *explicitSelection < candidates.size()) {
        return explicitSelection;
    }

    const bool hasCompletion = std::any_of(
        candidates.begin(), candidates.end(), [](const Candidate& candidate) {
            return (candidate.sourceFlags & CandidateSourceCompletion) != 0;
        });
    if (hasCompletion) return std::nullopt;

    for (const auto& candidate : candidates) {
        if (candidate.isOriginal && candidate.isProtected) {
            return std::nullopt;
        }
    }

    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!IsSafeAutomaticCandidate(rawText, candidates[index])) continue;
        if (selected) return std::nullopt;
        selected = index;
    }
    return selected;
}

bool AutoApplyPolicy::IsSafeAutomaticCandidate(std::wstring_view rawText,
                                               const Candidate& candidate) const noexcept {
    if (candidate.isOriginal || candidate.isProtected || candidate.text == rawText) {
        return false;
    }
    if ((candidate.policyFlags & (CandidatePolicyProtect | CandidatePolicySuggestOnly)) != 0) {
        return false;
    }
    if ((candidate.policyFlags & CandidatePolicyNormalize) != 0) {
        return true;
    }
    if ((candidate.policyFlags & CandidatePolicyCorrect) == 0) return false;

    constexpr std::uint32_t highConfidenceSourceFlags =
        CandidateSourceSingleEdit | CandidateSourceRepeatedKey |
        CandidateSourceTransposition | CandidateSourceMissingCharacter |
        CandidateSourceExtraCharacter;
    return (candidate.sourceFlags & highConfidenceSourceFlags) != 0;
}

}  // namespace tekito
