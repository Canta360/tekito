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
    [[nodiscard]] std::optional<std::wstring> Recheck(std::wstring_view word, std::wstring_view precedingText,
                                                      std::wstring_view followingText) const override;
    [[nodiscard]] std::optional<std::wstring> Revise(std::wstring_view typed, std::wstring_view applied,
                                                     std::wstring_view precedingText,
                                                     std::wstring_view followingText) const override;
    void RefreshUserDictionary() override;
    [[nodiscard]] std::vector<Candidate> Generate(std::wstring_view rawText) const;

private:
    struct Providers;

    // Today's date for "today" and the like, after the word as typed.
    static void AddDates(std::wstring_view rawText, std::vector<Candidate>& candidates);
    // Symbols by the word's English name (heart: the outlined and filled hearts), after its emoji.
    static void AddSymbols(std::wstring_view rawText, std::vector<Candidate>& candidates);
    // A number word in its other forms (three: 3, ３, 三, Ⅲ, ③), after its emoji.
    static void AddNumbers(std::wstring_view rawText, std::vector<Candidate>& candidates);
    // English typed without spaces, with its spaces (ConversionOptions::spacelessEnglish).
    void AddRunTogether(std::wstring_view rawText, const ConversionOptions& options,
                        std::vector<Candidate>& candidates) const;

    CandidateEngine(const UserDictionary* dictionary,
                    const IUserLearningProvider& learningProvider,
                    const SocialLearningProvider& socialLearningProvider);

    std::unique_ptr<Providers> providers_;
    std::unique_ptr<CandidateGenerator> generator_;
};

}  // namespace tekito
