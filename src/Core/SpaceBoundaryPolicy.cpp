#include "Core/SpaceBoundaryPolicy.h"

#include "Core/TypoModel.h"

namespace tekito {

std::optional<std::size_t> SpaceBoundaryPolicy::SelectCorrection(
    std::wstring_view rawText, std::span<const Candidate> candidates) const noexcept {
    if (candidates.empty() || !IsAcceptableCorrection(rawText, candidates.front())) {
        return std::nullopt;
    }
    // A word the user keeps as typed is never replaced.
    for (const auto& candidate : candidates) {
        const bool kept = candidate.isProtected || (candidate.policyFlags & CandidatePolicyProtect) != 0;
        if (candidate.isOriginal && kept) {
            return std::nullopt;
        }
    }
    return 0;
}

bool SpaceBoundaryPolicy::IsAcceptableCorrection(std::wstring_view rawText,
                                                  const Candidate& candidate) const noexcept {
    if (candidate.isOriginal || candidate.isProtected || candidate.text == rawText ||
        candidate.label == SemanticLabel::ProperNoun ||
        candidate.label == SemanticLabel::Slang ||
        candidate.label == SemanticLabel::Abbreviation ||
        candidate.label == SemanticLabel::Phrase || candidate.label == SemanticLabel::Emoji) {
        return false;
    }

    constexpr std::uint32_t rejectedPolicyFlags =
        CandidatePolicyProtect | CandidatePolicySuggestOnly | CandidatePolicyExpand |
        CandidatePolicyContext | CandidatePolicyEmoji;
    if ((candidate.policyFlags & rejectedPolicyFlags) != 0) return false;

    constexpr std::uint32_t acceptedPolicyFlags =
        CandidatePolicyNormalize | CandidatePolicyCorrect;
    if ((candidate.policyFlags & acceptedPolicyFlags) == 0) return false;

    constexpr std::uint32_t rejectedSourceFlags =
        CandidateSourceCompletion | CandidateSourceEmoji | CandidateSourceProtectedPattern |
        CandidateSourceTekitoOwnedSlang;
    if ((candidate.sourceFlags & rejectedSourceFlags) != 0) return false;
    return IsLikelyMeant(rawText, candidate);
}

bool IsLikelyMeant(std::wstring_view rawText, const Candidate& candidate) noexcept {
    // The same word written another way (case, an apostrophe), a common
    // misspelling put right, the user's own words, words run together or
    // doubled: as before.
    if ((candidate.policyFlags & CandidatePolicyNormalize) != 0) return true;
    constexpr std::uint32_t knownSourceFlags =
        CandidateSourceWikipediaMisspelling | CandidateSourceQwertyTypoCatalog | CandidateSourceUserDictionary |
        CandidateSourceRunTogether | CandidateSourceDoubledWord;
    if ((candidate.sourceFlags & knownSourceFlags) != 0) return true;
    // Otherwise only slips a hand makes: a word the dictionary lacks
    // (linpeas, kaggle, fcitx5) stays as typed, the word offered in the list.
    return IsPlausibleSlip(rawText, candidate.text);
}

}  // namespace tekito
