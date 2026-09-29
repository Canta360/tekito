#include "Core/CandidateEngine.h"

#include "Core/BuiltinLexicon.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/ExternalSlangProvider.h"
#include "Core/MisspellingProvider.h"
#include "Core/SpecialConversions.h"
#include "Core/UserDictionary.h"
#include "Core/UserLearning.h"
#include "Core/UserLexiconProvider.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <mutex>

namespace tekito {
namespace {

// Everything the default engine reads from Data Packs. Loading it takes
// seconds and hundreds of MB, and it is read-only afterwards (the providers
// that read files lock internally), so one copy serves every engine in the
// process -- a host application with several UI threads, each with its own
// text service, must not load it once per thread.
struct SharedProviders {
    ExternalLexiconProvider externalProvider;
    ExternalSlangProvider slangProvider;
    ExternalSlangProvider wiktionarySlangProvider;
    ExternalSlangProvider properNounProvider;
    ExternalSlangProvider emojiProvider;
    ExternalSlangProvider socialExpressionProvider;
    ExternalSlangProvider japanesePhoneticProvider;
    WikipediaCommonMisspellingsProvider wikipediaProvider;
    ExternalFrequencyProvider frequencyProvider;
    ExternalPhraseContextProvider phraseProvider;
    BuiltinLexiconProvider builtinProvider;
    CompositeLexiconProvider externalWithFallback;
    CompositeLexiconProvider wiktionaryWithSlang;
    CompositeLexiconProvider emojiWithExpressions;
    CompositeLexiconProvider socialWithExpressions;
    CompositeLexiconProvider phoneticWithExpressions;
    CompositeLexiconProvider supplementaryProvider;

    SharedProviders()
        : externalProvider(ExternalLexiconProvider::DefaultPath()),
          slangProvider(ExternalSlangProvider::DefaultPath()),
          wiktionarySlangProvider(ExternalLexiconProvider::DataPackRoot() /
                                      L"wiktionary-slang" / L"entries.tsv",
                                  CandidateSourceWiktionary),
          properNounProvider(ExternalLexiconProvider::DataPackRoot() /
                                 L"proper-nouns" / L"entries.tsv",
                             CandidateSourceProperNoun),
          emojiProvider(ExternalLexiconProvider::DataPackRoot() /
                            L"emoji" / L"entries.tsv",
                        CandidateSourceEmoji),
          socialExpressionProvider(ExternalLexiconProvider::DataPackRoot() /
                                       L"social-expression" / L"entries.tsv",
                                   CandidateSourceSocialExpression),
          japanesePhoneticProvider(ExternalLexiconProvider::DataPackRoot() /
                                       L"japanese-phonetic" / L"entries.tsv",
                                   CandidateSourceJapanesePhonetic),
          wikipediaProvider(WikipediaCommonMisspellingsProvider::DefaultPath()),
          frequencyProvider(ExternalFrequencyProvider::DefaultPath()),
          phraseProvider(ExternalPhraseContextProvider::DefaultPath()),
          externalWithFallback(externalProvider, builtinProvider),
          wiktionaryWithSlang(wiktionarySlangProvider, slangProvider),
          emojiWithExpressions(emojiProvider, wiktionaryWithSlang),
          socialWithExpressions(socialExpressionProvider, emojiWithExpressions),
          phoneticWithExpressions(japanesePhoneticProvider, socialWithExpressions),
          supplementaryProvider(properNounProvider, phoneticWithExpressions) {}
};

std::mutex g_sharedProvidersMutex;
std::shared_ptr<const SharedProviders> g_sharedProviders;  // kept for the life of the process
std::atomic<bool> g_sharedProvidersLoaded{false};

std::shared_ptr<const SharedProviders> AcquireSharedProviders() {
    // Held while loading, so concurrent callers wait for the one load
    // instead of each starting their own.
    std::lock_guard lock(g_sharedProvidersMutex);
    if (!g_sharedProviders) {
        g_sharedProviders = std::make_shared<const SharedProviders>();
        g_sharedProvidersLoaded.store(true, std::memory_order_release);
    }
    return g_sharedProviders;
}

const IUserLearningProvider& NoUserLearning() {
    static const NullUserLearningProvider provider;
    return provider;
}

const SocialLearningProvider& NoSocialLearning() {
    static const SocialLearningStore store;
    static const SocialLearningProvider provider(store);
    return provider;
}

}  // namespace

void PreloadDefaultEngineData() {
    (void)AcquireSharedProviders();
}

bool IsDefaultEngineDataLoaded() noexcept {
    return g_sharedProvidersLoaded.load(std::memory_order_acquire);
}

// Per-engine part: only the user's dictionary, layered over the shared data.
struct CandidateEngine::Providers {
    std::shared_ptr<const SharedProviders> shared;
    UserDictionary emptyUserDictionary;
    UserLexiconProvider userProvider;
    CompositeLexiconProvider provider;

    explicit Providers(const UserDictionary* dictionary)
        : shared(AcquireSharedProviders()),
          userProvider(dictionary ? *dictionary : emptyUserDictionary),
          provider(userProvider, shared->externalWithFallback) {}
};

CandidateEngine::CandidateEngine()
    : CandidateEngine(nullptr, NoUserLearning(), NoSocialLearning()) {}

CandidateEngine::CandidateEngine(const ILexiconProvider& lexiconProvider)
    : generator_(std::make_unique<CandidateGenerator>(lexiconProvider)) {}

CandidateEngine::CandidateEngine(const UserDictionary& dictionary)
    : CandidateEngine(&dictionary, NoUserLearning(), NoSocialLearning()) {}

CandidateEngine::CandidateEngine(const UserDictionary& dictionary,
                                 const IUserLearningProvider& learningProvider)
    : CandidateEngine(&dictionary, learningProvider, NoSocialLearning()) {}

CandidateEngine::CandidateEngine(const UserDictionary& dictionary,
                                 const IUserLearningProvider& learningProvider,
                                 const SocialLearningProvider& socialLearningProvider)
    : CandidateEngine(&dictionary, learningProvider, socialLearningProvider) {}

CandidateEngine::CandidateEngine(const UserDictionary* dictionary,
                                 const IUserLearningProvider& learningProvider,
                                 const SocialLearningProvider& socialLearningProvider)
    : providers_(std::make_unique<Providers>(dictionary)),
      generator_(std::make_unique<CandidateGenerator>(
          providers_->provider, providers_->shared->supplementaryProvider,
          providers_->shared->wikipediaProvider, providers_->shared->frequencyProvider,
          providers_->shared->phraseProvider, learningProvider, socialLearningProvider)) {}

CandidateEngine::~CandidateEngine() = default;

void CandidateEngine::RefreshUserDictionary() {
    if (!providers_) return;
    // The top-level composite caches the user entries it was built from.
    providers_->userProvider.Refresh();
    providers_->provider.Refresh();
}

ConversionResult CandidateEngine::Convert(const ConversionRequest& request) const {
    if (!generator_) return {};

    auto rawText = request.rawText;
    const bool engineCapitalized = request.capitalization.origin ==
                                   CapitalizationOrigin::EngineApplied;
    if (engineCapitalized && !rawText.empty()) {
        rawText.front() = static_cast<wchar_t>(std::towlower(rawText.front()));
    }

    auto candidates = generator_->Generate(rawText, request.context.precedingText,
                                           request.context.followingText,
                                           request.options);
    if (engineCapitalized) {
        for (auto& candidate : candidates) {
            if (!candidate.text.empty() && !candidate.isProtected &&
                candidate.label != SemanticLabel::ProperNoun) {
                candidate.text.front() =
                    static_cast<wchar_t>(std::towupper(candidate.text.front()));
            }
        }
    }
    if (request.options.special.dates) AddDates(rawText, candidates);
    return {std::move(candidates)};
}

void CandidateEngine::AddDates(std::wstring_view rawText, std::vector<Candidate>& candidates) {
    std::wstring word(rawText);
    for (auto& c : word) c = static_cast<wchar_t>(std::towlower(c));
    auto dates = SpecialConversions::Installed().Dates(SpecialConversions::Language::English, word,
                                                       LocalTime::Now());
    if (dates.empty() || candidates.empty()) return;
    // Right after the word as typed, which stays first: a date is only
    // ever offered.
    const auto original = std::find_if(candidates.begin(), candidates.end(),
                                       [](const Candidate& candidate) { return candidate.isOriginal; });
    auto at = original == candidates.end() ? candidates.begin() + 1 : original + 1;
    for (auto& date : dates) {
        Candidate candidate;
        candidate.text = std::move(date);
        candidate.isProtected = true;
        candidate.policyFlags = CandidatePolicySuggestOnly;
        at = candidates.insert(at, std::move(candidate)) + 1;
    }
}

std::vector<Candidate> CandidateEngine::Generate(std::wstring_view rawText) const {
    return generator_ ? generator_->Generate(rawText) : std::vector<Candidate>{};
}

}  // namespace tekito
