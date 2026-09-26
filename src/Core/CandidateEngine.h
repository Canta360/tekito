#pragma once

#include "Core/CandidateGenerator.h"
#include "Core/ConversionEngine.h"

#include <memory>
#include <string_view>
#include <vector>

namespace tekito {

class UserDictionary;
class SocialLearningProvider;

// The conversion engine TEKITO ships: candidates from the installed Data
// Packs, the user's dictionary and what TEKITO has learned from the user.
class CandidateEngine final : public IConversionEngine {
public:
    CandidateEngine();
    explicit CandidateEngine(const ILexiconProvider& lexiconProvider);
    explicit CandidateEngine(const UserDictionary& dictionary);
    CandidateEngine(const UserDictionary& dictionary,
                    const IUserLearningProvider& learningProvider);
    CandidateEngine(const UserDictionary& dictionary,
                    const IUserLearningProvider& learningProvider,
                    const SocialLearningProvider& socialLearningProvider);
    ~CandidateEngine();

    [[nodiscard]] ConversionResult Convert(const ConversionRequest& request) const override;
    void RefreshUserDictionary() override;
    [[nodiscard]] std::vector<Candidate> Generate(std::wstring_view rawText) const;

private:
    struct Providers;

    CandidateEngine(const UserDictionary* dictionary,
                    const IUserLearningProvider& learningProvider,
                    const SocialLearningProvider& socialLearningProvider);

    std::unique_ptr<Providers> providers_;
    std::unique_ptr<CandidateGenerator> generator_;
};

}  // namespace tekito
