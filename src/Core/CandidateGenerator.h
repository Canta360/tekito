#pragma once

#include "Core/Candidate.h"
#include "Core/CandidateRanker.h"
#include "Core/ConversionEngine.h"
#include "Core/LexiconProvider.h"
#include "Core/MisspellingProvider.h"
#include "Core/PolicyEngine.h"
#include "Core/TypoModel.h"

#include <string_view>
#include <vector>

namespace tekito {

class SocialLearningProvider;

class CandidateGenerator final {
public:
    explicit CandidateGenerator(const ILexiconProvider& lexiconProvider);
    CandidateGenerator(const ILexiconProvider& lexiconProvider,
                       const IMisspellingProvider& misspellingProvider);
    CandidateGenerator(const ILexiconProvider& lexiconProvider,
                       const IFrequencyProvider& frequencyProvider,
                       const IPhraseContextProvider& phraseProvider);
    CandidateGenerator(const ILexiconProvider& lexiconProvider,
                       const IMisspellingProvider& misspellingProvider,
                       const IFrequencyProvider& frequencyProvider,
                       const IPhraseContextProvider& phraseProvider,
                       const IUserLearningProvider& learningProvider);
    CandidateGenerator(const ILexiconProvider& lexiconProvider,
                       const ILexiconProvider& supplementaryProvider,
                       const IMisspellingProvider& misspellingProvider,
                       const IFrequencyProvider& frequencyProvider,
                       const IPhraseContextProvider& phraseProvider,
                       const IUserLearningProvider& learningProvider);
    CandidateGenerator(const ILexiconProvider& lexiconProvider,
                       const ILexiconProvider& supplementaryProvider,
                       const IMisspellingProvider& misspellingProvider,
                       const IFrequencyProvider& frequencyProvider,
                       const IPhraseContextProvider& phraseProvider,
                       const IUserLearningProvider& learningProvider,
                       const SocialLearningProvider& socialLearningProvider);

    [[nodiscard]] std::vector<Candidate> Generate(
        std::wstring_view rawText, std::wstring_view context = {},
        std::wstring_view followingContext = {},
        const ConversionOptions& options = {}) const;

private:
    // `flags` for `candidate`, except that a correction the user undid for
    // this input is only offered from then on.
    [[nodiscard]] std::uint32_t Offered(std::wstring_view lowerRaw, std::wstring_view candidate,
                                        std::uint32_t flags) const;

    const ILexiconProvider& lexiconProvider_;
    const ILexiconProvider* supplementaryProvider_{nullptr};
    const IMisspellingProvider& misspellingProvider_;
    const IFrequencyProvider& frequencyProvider_;
    const IPhraseContextProvider& phraseProvider_;
    const IUserLearningProvider& learningProvider_;
    const SocialLearningProvider* socialLearningProvider_{nullptr};
    PolicyEngine policyEngine_;
    TypoModel typoModel_;
    CandidateRanker ranker_;
};

}  // namespace tekito
