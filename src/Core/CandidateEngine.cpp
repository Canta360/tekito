#include "Core/CandidateEngine.h"

#include "Core/AutoApplyPolicy.h"
#include "Core/BuiltinLexicon.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/ExternalSlangProvider.h"
#include "Core/MisspellingProvider.h"
#include "Core/RunTogether.h"
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
    // Names from Wikidata (english-names): people, companies, brands, works.
    ExternalSlangProvider namesProvider;
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
    CompositeLexiconProvider namesWithPhonetic;
    CompositeLexiconProvider supplementaryProvider;

    SharedProviders()
        : externalProvider(ExternalLexiconProvider::DefaultPath()),
          slangProvider(ExternalSlangProvider::DefaultPath()),
          wiktionarySlangProvider(ExternalLexiconProvider::PackDirectory(L"wiktionary-slang") / L"entries.tsv",
                                  CandidateSourceWiktionary),
          properNounProvider(ExternalLexiconProvider::PackDirectory(L"proper-nouns") / L"entries.tsv",
                             CandidateSourceProperNoun),
          namesProvider(ExternalLexiconProvider::PackDirectory(L"english-names") / L"entries.tsv",
                        CandidateSourceProperNoun),
          emojiProvider(ExternalLexiconProvider::PackDirectory(L"emoji") / L"entries.tsv",
                        CandidateSourceEmoji),
          socialExpressionProvider(ExternalLexiconProvider::PackDirectory(L"social-expression") / L"entries.tsv",
                                   CandidateSourceSocialExpression),
          japanesePhoneticProvider(ExternalLexiconProvider::PackDirectory(L"japanese-phonetic") / L"entries.tsv",
                                   CandidateSourceJapanesePhonetic),
          wikipediaProvider(WikipediaCommonMisspellingsProvider::DefaultPath()),
          frequencyProvider(ExternalFrequencyProvider::DefaultPath()),
          phraseProvider(ExternalPhraseContextProvider::DefaultPath()),
          externalWithFallback(externalProvider, builtinProvider),
          wiktionaryWithSlang(wiktionarySlangProvider, slangProvider),
          emojiWithExpressions(emojiProvider, wiktionaryWithSlang),
          socialWithExpressions(socialExpressionProvider, emojiWithExpressions),
          phoneticWithExpressions(japanesePhoneticProvider, socialWithExpressions),
          namesWithPhonetic(namesProvider, phoneticWithExpressions),
          supplementaryProvider(properNounProvider, namesWithPhonetic) {}
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
    if (request.options.spacelessEnglish) AddRunTogether(rawText, request.options, candidates);
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
    if (request.options.special.symbols) AddSymbols(rawText, candidates);
    if (request.options.special.numbers) AddNumbers(rawText, candidates);
    // Numbered as listed, with what was added.
    for (std::size_t i = 0; i < candidates.size(); ++i) candidates[i].id = static_cast<std::uint32_t>(i + 1);
    return {std::move(candidates)};
}

std::optional<std::wstring> CandidateEngine::Recheck(std::wstring_view word, std::wstring_view precedingText,
                                                     std::wstring_view followingText) const {
    if (!generator_) return std::nullopt;
    return generator_->Recheck(word, precedingText, followingText);
}

std::optional<std::wstring> CandidateEngine::Revise(std::wstring_view typed, std::wstring_view applied,
                                                    std::wstring_view precedingText,
                                                    std::wstring_view followingText) const {
    if (!generator_) return std::nullopt;
    return generator_->Revise(typed, applied, precedingText, followingText);
}

void CandidateEngine::AddRunTogether(std::wstring_view rawText, const ConversionOptions& options,
                                     std::vector<Candidate>& candidates) const {
    // Long enough to be words; not a word the list has (therapist is not
    // the rapist). Words of three letters or more this well known, shorter
    // ones common; Space puts the spaces in when every word is this well
    // known (tomorrow, not github).
    constexpr std::size_t kShortest = 8;
    constexpr std::size_t kShortWord = 2;
    constexpr double kListed = 0.9;
    constexpr double kKnown = 2.5;
    constexpr double kCommon = 4.0;
    constexpr double kApply = 3.5;
    if (rawText.size() < kShortest || !std::all_of(rawText.begin(), rawText.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; })) {
        return;
    }
    const auto& frequency = providers_->shared->frequencyProvider;
    if (frequency.Score(rawText) >= kListed) return;
    const auto score = [&](std::wstring_view word) { return frequency.Score(word); };
    // One letter only as common as a and i (engineerss is not engineers s;
    // the next, x and s, are at 4.3).
    constexpr double kOneLetterWord = 5.0;
    const auto known = [&](std::wstring_view word) {
        if (word.size() == 1) return score(word) >= kOneLetterWord;
        return score(word) >= (word.size() <= kShortWord ? kCommon : kKnown);
    };
    const auto words = SplitRunTogether(rawText, known, score);
    if (!words || words->size() < 2) return;
    // Two words are only offered (apartmet: apart met is a slip as likely).
    constexpr std::size_t kApplyWords = 3;
    const bool common = words->size() >= kApplyWords &&
                        std::all_of(words->begin(), words->end(), [&](const std::wstring& word) { return score(word) >= kApply; });
    // Each word as Space writes it alone (Monday, I).
    std::wstring text;
    for (const auto& word : *words) {
        std::wstring written = word;
        for (const auto& candidate : generator_->Generate(word)) {
            const auto& t = candidate.text;
            bool same = t.size() == word.size() && t != word && (candidate.policyFlags & CandidatePolicyNormalize) != 0;
            for (std::size_t i = 0; i < t.size() && same; ++i) same = std::towlower(t[i]) == word[i];
            if (same) {
                written = t;
                break;
            }
        }
        text += (text.empty() ? L"" : L" ") + written;
    }
    Candidate candidate;
    candidate.text = std::move(text);
    candidate.label = SemanticLabel::Suggestion;
    candidate.sourceFlags = CandidateSourceRunTogether;
    // A typo one edit away comes first (apartmet: apartment, staright:
    // straight, not st a right), unless the spaces are always put in.
    constexpr std::uint32_t kOneEdit = CandidateSourceSingleEdit | CandidateSourceRepeatedKey | CandidateSourceTransposition |
                                       CandidateSourceMissingCharacter | CandidateSourceExtraCharacter;
    const bool corrected = AutoApplyPolicy().SelectForBoundary(rawText, candidates).has_value() ||
                           std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c) {
                               return !c.isOriginal && (c.policyFlags & CandidatePolicyCorrect) != 0 && (c.sourceFlags & kOneEdit) != 0;
                           });
    const bool apply = options.spacelessEnglishAlways || (common && !corrected);
    candidate.policyFlags = apply ? CandidatePolicyNormalize : CandidatePolicySuggestOnly;
    if (apply) {
        // Space puts this in: the other choices are only offered.
        for (auto& other : candidates) {
            if (!other.isOriginal) other.policyFlags |= CandidatePolicySuggestOnly;
        }
        candidates.insert(candidates.begin(), std::move(candidate));
        return;
    }
    const auto original = std::find_if(candidates.begin(), candidates.end(),
                                       [](const Candidate& c) { return c.isOriginal; });
    candidates.insert(original == candidates.end() ? candidates.end() : original + 1, std::move(candidate));
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

void CandidateEngine::AddSymbols(std::wstring_view rawText, std::vector<Candidate>& candidates) {
    std::wstring word(rawText);
    for (auto& c : word) c = static_cast<wchar_t>(std::towlower(c));
    auto symbols = SpecialConversions::Installed().SymbolsNamed(word);
    if (symbols.empty() || candidates.empty()) return;
    // After the word as typed and its emoji: a symbol is only ever offered.
    auto at = std::find_if(candidates.begin(), candidates.end(),
                           [](const Candidate& candidate) { return candidate.isOriginal; });
    at = at == candidates.end() ? candidates.begin() + 1 : at + 1;
    while (at != candidates.end() && at->label == SemanticLabel::Emoji) ++at;
    for (auto& symbol : symbols) {
        const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const Candidate& candidate) { return candidate.text == symbol; });
        if (seen) continue;
        Candidate candidate;
        candidate.text = std::move(symbol);
        candidate.isProtected = true;
        candidate.policyFlags = CandidatePolicySuggestOnly;
        at = candidates.insert(at, std::move(candidate)) + 1;
    }
}

void CandidateEngine::AddNumbers(std::wstring_view rawText, std::vector<Candidate>& candidates) {
    const auto digits = SpecialConversions::EnglishNumber(rawText);
    if (!digits || candidates.empty()) return;
    auto numbers = SpecialConversions::Installed().Numbers(*digits);
    numbers.insert(numbers.begin(), *digits);
    // After the word as typed and its emoji, as symbols are: a number is
    // only ever offered.
    auto at = std::find_if(candidates.begin(), candidates.end(),
                           [](const Candidate& candidate) { return candidate.isOriginal; });
    at = at == candidates.end() ? candidates.begin() + 1 : at + 1;
    while (at != candidates.end() && at->label == SemanticLabel::Emoji) ++at;
    for (auto& number : numbers) {
        const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const Candidate& candidate) { return candidate.text == number; });
        if (seen) continue;
        Candidate candidate;
        candidate.text = std::move(number);
        candidate.isProtected = true;
        candidate.policyFlags = CandidatePolicySuggestOnly;
        at = candidates.insert(at, std::move(candidate)) + 1;
    }
}

std::vector<Candidate> CandidateEngine::Generate(std::wstring_view rawText) const {
    return generator_ ? generator_->Generate(rawText) : std::vector<Candidate>{};
}

}  // namespace tekito
