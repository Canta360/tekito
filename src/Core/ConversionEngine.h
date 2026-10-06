#pragma once

#include "Core/Candidate.h"
#include "Core/SpecialConversions.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {

class UserDictionary;
class IUserLearningProvider;
class SocialLearningProvider;

struct Context {
    bool sentenceStart{false};
    // Optional preceding text used only as a local phrase-ranking key.
    std::wstring precedingText;
    // Optional following text available from the host document.
    std::wstring followingText;
};

enum class CapitalizationOrigin {
    Unknown,
    UserTyped,
    EngineApplied,
};

struct CapitalizationInfo {
    CapitalizationOrigin origin{CapitalizationOrigin::Unknown};
};

struct ConversionOptions {
    bool correctionEnabled{true};
    bool commonMisspellingsEnabled{true};
    bool contextSuggestionsEnabled{true};
    bool completionEnabled{true};
    bool japanesePhoneticSuggestionsEnabled{true};
    int socialExpressionRange{1};
    // Words that often follow the words before it, offered while typing one.
    bool nextWordPrediction{true};
    // A word typed twice in a row ("the the") is taken out; one that may be
    // meant twice ("that that") only offers it.
    bool doubledWords{true};
    // English uses the dates ("today"); the rest is typed outside words.
    SpecialConversionOptions special;
};

struct ConversionRequest {
    std::wstring rawText;
    Context context;
    CapitalizationInfo capitalization;
    ConversionOptions options;
};

struct ConversionResult {
    std::vector<Candidate> candidates;
};

[[nodiscard]] bool IsSentenceStart(std::wstring_view precedingText) noexcept;
[[nodiscard]] bool NeedsTerminalPeriod(std::wstring_view lineText) noexcept;

class IConversionEngine {
public:
    virtual ~IConversionEngine() = default;
    [[nodiscard]] virtual ConversionResult Convert(const ConversionRequest& request) const = 0;
    // Re-reads the UserDictionary the engine was created with, after it was
    // changed in place. Far cheaper than building a new engine, which
    // reloads every Data Pack.
    virtual void RefreshUserDictionary() {}
};

// The default engine's Data Packs are loaded once per process and shared by
// every engine created afterwards. Loading takes a few seconds, so callers on
// a UI thread should preload on a worker thread and create engines once
// IsDefaultEngineDataLoaded() is true (creating one is then cheap).
void PreloadDefaultEngineData();
[[nodiscard]] bool IsDefaultEngineDataLoaded() noexcept;

[[nodiscard]] std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine() noexcept;
[[nodiscard]] std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine(
    const UserDictionary& dictionary) noexcept;
[[nodiscard]] std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine(
    const UserDictionary& dictionary,
    const IUserLearningProvider& learningProvider) noexcept;
[[nodiscard]] std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine(
    const UserDictionary& dictionary,
    const IUserLearningProvider& learningProvider,
    const SocialLearningProvider& socialLearningProvider) noexcept;

}  // namespace tekito
