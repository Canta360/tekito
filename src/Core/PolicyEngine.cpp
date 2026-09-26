#include "Core/PolicyEngine.h"

#include "Core/Candidate.h"

namespace tekito {

bool PolicyEngine::IsTekitoOwnedExpression(SemanticLabel label) noexcept {
    return label == SemanticLabel::Slang || label == SemanticLabel::Abbreviation ||
           label == SemanticLabel::Phrase;
}

std::uint32_t PolicyEngine::RawFlags(const LexiconEntry& entry) const noexcept {
    if (entry.rawPolicyFlags != CandidatePolicyNone) return entry.rawPolicyFlags;
    if (IsTekitoOwnedExpression(entry.rawLabel)) return CandidatePolicyProtect;
    if (!entry.originalFirst) return CandidatePolicySuggestOnly;
    return CandidatePolicyNone;
}

std::uint32_t PolicyEngine::CandidateFlags(const LexiconEntry& entry) const noexcept {
    if (entry.candidatePolicyFlags != CandidatePolicyNone) return entry.candidatePolicyFlags;
    if (entry.candidateLabel == SemanticLabel::Emoji) {
        return CandidatePolicyEmoji | CandidatePolicySuggestOnly;
    }
    if (entry.candidateLabel == SemanticLabel::Standard) {
        return CandidatePolicyExpand | CandidatePolicySuggestOnly;
    }
    if (!entry.originalFirst) return CandidatePolicyNormalize;
    return CandidatePolicyNone;
}

}  // namespace tekito
