#include "Core/CandidateGenerator.h"

#include "Core/UserLearning.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>

namespace tekito {
namespace {

std::wstring Lower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

std::vector<std::wstring> ContextWords(std::wstring_view value) {
    std::vector<std::wstring> words;
    std::wstring current;
    for (const wchar_t ch : value) {
        if (std::iswalpha(ch) || ch == L'\'' || ch == L'\u2019') {
            current.push_back(ch == L'\u2019' ? L'\'' : static_cast<wchar_t>(std::towlower(ch)));
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    return words;
}

std::wstring JoinWords(const std::vector<std::wstring>& words, std::size_t start,
                       std::size_t count) {
    std::wstring result;
    for (std::size_t index = start; index < start + count && index < words.size(); ++index) {
        if (!result.empty()) result.push_back(L' ');
        result += words[index];
    }
    return result;
}

double ContextScore(const IPhraseContextProvider& provider, std::wstring_view word,
                    const std::vector<std::wstring>& previousWords,
                    const std::vector<std::wstring>& followingWords) {
    constexpr double kSingleWordContextWeight = 0.25;
    const auto target = Lower(word);
    double previousScore = 0.0;
    std::size_t previousCount = 0;
    for (std::size_t count = 1; count <= std::min<std::size_t>(2, previousWords.size()); ++count) {
        const auto contextScore = provider.Score(
            target, JoinWords(previousWords, previousWords.size() - count, count));
        if (contextScore > 0.0) {
            previousScore = contextScore;
            previousCount = count;
        }
    }

    double followingScore = 0.0;
    std::size_t followingCount = 0;
    for (std::size_t count = 1; count <= std::min<std::size_t>(2, followingWords.size()); ++count) {
        const auto context = target + (count > 1 ? L" " + followingWords[0] : L"");
        const auto contextScore = provider.Score(followingWords[count - 1], context);
        if (contextScore > 0.0) {
            followingScore = contextScore;
            followingCount = count;
        }
    }
    const auto score = previousScore + followingScore;
    return std::max(previousCount, followingCount) <= 1
               ? score * kSingleWordContextWeight
               : score;
}

bool IsAllLower(std::wstring_view value) {
    return std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return !std::iswalpha(ch) || std::iswlower(ch);
    });
}

bool IsCapitalized(std::wstring_view value) {
    return value.size() > 1 && std::iswupper(value.front()) &&
           std::all_of(value.begin() + 1, value.end(), [](wchar_t ch) {
               return !std::iswalpha(ch) || std::iswlower(ch);
           });
}

// A word typed with a second capital by mistake: "THe", "MOnday" (two
// capitals, then only small letters).
bool HasTwoInitialCapitals(std::wstring_view value) {
    if (value.size() < 3 || !std::iswupper(value[0]) || !std::iswupper(value[1])) return false;
    return std::all_of(value.begin() + 2, value.end(), [](wchar_t ch) { return std::iswlower(ch) != 0; });
}

struct CasingRule {
    std::wstring_view lower;
    std::wstring_view written;
    bool automatic;
};

// Words always written with capitals: days, months, languages and
// nationalities, holidays and a few product names. Those that are also
// everyday small words (march, may, august, polish, china, turkey) are
// only offered.
constexpr CasingRule kCasingRules[] = {
    {L"monday", L"Monday", true}, {L"tuesday", L"Tuesday", true}, {L"wednesday", L"Wednesday", true},
    {L"thursday", L"Thursday", true}, {L"friday", L"Friday", true}, {L"saturday", L"Saturday", true},
    {L"sunday", L"Sunday", true},
    {L"january", L"January", true}, {L"february", L"February", true}, {L"march", L"March", false},
    {L"april", L"April", true}, {L"may", L"May", false}, {L"june", L"June", true}, {L"july", L"July", true},
    {L"august", L"August", false}, {L"september", L"September", true}, {L"october", L"October", true},
    {L"november", L"November", true}, {L"december", L"December", true},
    {L"english", L"English", true}, {L"japanese", L"Japanese", true}, {L"chinese", L"Chinese", true},
    {L"korean", L"Korean", true}, {L"french", L"French", true}, {L"german", L"German", true},
    {L"spanish", L"Spanish", true}, {L"italian", L"Italian", true}, {L"russian", L"Russian", true},
    {L"portuguese", L"Portuguese", true}, {L"dutch", L"Dutch", true}, {L"arabic", L"Arabic", true},
    {L"hindi", L"Hindi", true}, {L"thai", L"Thai", true}, {L"vietnamese", L"Vietnamese", true},
    {L"indonesian", L"Indonesian", true}, {L"greek", L"Greek", true}, {L"swedish", L"Swedish", true},
    {L"norwegian", L"Norwegian", true}, {L"danish", L"Danish", true}, {L"finnish", L"Finnish", true},
    {L"turkish", L"Turkish", true}, {L"hebrew", L"Hebrew", true}, {L"latin", L"Latin", true},
    {L"polish", L"Polish", false}, {L"american", L"American", true}, {L"british", L"British", true},
    {L"european", L"European", true}, {L"asian", L"Asian", true}, {L"african", L"African", true},
    {L"canadian", L"Canadian", true}, {L"australian", L"Australian", true}, {L"mexican", L"Mexican", true},
    {L"brazilian", L"Brazilian", true}, {L"indian", L"Indian", true}, {L"irish", L"Irish", true},
    {L"scottish", L"Scottish", true}, {L"welsh", L"Welsh", true},
    {L"japan", L"Japan", true}, {L"china", L"China", false}, {L"turkey", L"Turkey", false},
    {L"christmas", L"Christmas", true}, {L"easter", L"Easter", true}, {L"halloween", L"Halloween", true},
    {L"iphone", L"iPhone", true}, {L"ipad", L"iPad", true}, {L"ipod", L"iPod", true}, {L"imac", L"iMac", true},
    {L"macos", L"macOS", true}, {L"youtube", L"YouTube", true}, {L"github", L"GitHub", true},
    {L"linkedin", L"LinkedIn", true}, {L"paypal", L"PayPal", true}, {L"ebay", L"eBay", true},
    {L"javascript", L"JavaScript", true}, {L"typescript", L"TypeScript", true},
    {L"powerpoint", L"PowerPoint", true}, {L"playstation", L"PlayStation", true},
    {L"whatsapp", L"WhatsApp", true}, {L"tiktok", L"TikTok", true},
    {L"mcdonalds", L"McDonald's", true}, {L"mcdonald's", L"McDonald's", true},
};

// The written form for a small-letter word, with a plural or possessive
// ending kept (mondays, january's).
std::optional<std::pair<std::wstring, bool>> WrittenForm(std::wstring_view lower) {
    for (const auto& rule : kCasingRules) {
        if (lower == rule.lower) return std::pair{std::wstring(rule.written), rule.automatic};
    }
    for (const std::wstring_view ending : {std::wstring_view(L"'s"), std::wstring_view(L"s")}) {
        if (lower.size() <= ending.size() || !lower.ends_with(ending)) continue;
        const auto base = lower.substr(0, lower.size() - ending.size());
        for (const auto& rule : kCasingRules) {
            if (base == rule.lower && !rule.written.ends_with(L"'s")) {
                return std::pair{std::wstring(rule.written) + std::wstring(ending), rule.automatic};
            }
        }
    }
    return std::nullopt;
}

// `text` in the case `raw` was typed in; "I" stays a capital.
std::wstring WithCaseOf(std::wstring_view raw, std::wstring text) {
    const bool allUpper = std::any_of(raw.begin(), raw.end(), [](wchar_t ch) { return std::iswupper(ch) != 0; }) &&
                          std::none_of(raw.begin(), raw.end(), [](wchar_t ch) { return std::iswlower(ch) != 0; });
    if (allUpper && raw.size() > 1) {
        for (auto& ch : text) ch = static_cast<wchar_t>(std::towupper(ch));
    } else if (!raw.empty() && std::iswupper(raw.front()) && !text.empty()) {
        text.front() = static_cast<wchar_t>(std::towupper(text.front()));
    }
    return text;
}

// Takes `text` out of `output`, keeping what is known about it (it is put
// back elsewhere); a new candidate when it was not there.
Candidate TakeOut(std::vector<Candidate>& output, const std::wstring& text) {
    const auto found = std::find_if(output.begin(), output.end(),
                                    [&](const Candidate& candidate) { return !candidate.isOriginal && candidate.text == text; });
    if (found == output.end()) return Candidate{text, SemanticLabel::None, false, 0.0, false, CandidatePolicyNone,
                                                CandidateSourceNone};
    Candidate candidate = std::move(*found);
    output.erase(found);
    return candidate;
}

bool IsProtectedToken(std::wstring_view value) {
    bool hasUpper = false;
    bool hasLower = false;
    bool hasSpecial = false;
    for (wchar_t ch : value) {
        hasUpper = hasUpper || std::iswupper(ch);
        hasLower = hasLower || std::iswlower(ch);
        hasSpecial = hasSpecial || std::iswdigit(ch) || ch == L'-' || ch == L'_';
    }
    return hasSpecial || (hasUpper && hasLower) || (hasUpper && !hasLower);
}

std::wstring MatchCase(std::wstring_view raw, std::wstring text) {
    if (IsAllLower(raw)) return Lower(text);
    if (IsCapitalized(raw) && !text.empty()) {
        text = Lower(text);
        text.front() = static_cast<wchar_t>(std::towupper(text.front()));
    }
    return text;
}

std::uint32_t SourceFlagsFor(const BuiltinLexiconEntry& entry, SemanticLabel label) {
    if (entry.sourceFlags != CandidateSourceEmbeddedLexicon) return entry.sourceFlags;
    if (label == SemanticLabel::Emoji) return CandidateSourceEmoji;
    if (PolicyEngine::IsTekitoOwnedExpression(entry.rawLabel) || label == SemanticLabel::Standard) {
        return CandidateSourceTekitoOwnedSlang;
    }
    return CandidateSourceEmbeddedLexicon;
}

std::wstring DictionaryEntryIdFor(const BuiltinLexiconEntry& entry, SemanticLabel label) {
    if (label == SemanticLabel::None && entry.candidate.empty()) return {};
    const std::wstring_view prefix = entry.sourceFlags == CandidateSourceUserDictionary ? L"user:"
                                     : entry.sourceFlags == CandidateSourceExternalLexicon ? L"external:"
                                     : entry.sourceFlags == CandidateSourceWikipediaMisspelling ? L"wikipedia:"
                                     : entry.sourceFlags == CandidateSourceProperNoun ? L"proper:"
                                     : entry.sourceFlags == CandidateSourceWiktionary ? L"wiktionary:"
                                     : entry.sourceFlags == CandidateSourceSocialExpression ? L"social:"
                                     : entry.sourceFlags == CandidateSourceJapanesePhonetic ? L"phonetic:"
                                     : entry.sourceFlags == CandidateSourceQwertyTypoCatalog ? L"qwerty:"
                                     : label == SemanticLabel::Emoji       ? L"emoji:"
                                     : PolicyEngine::IsTekitoOwnedExpression(entry.rawLabel) ? L"tekito-owned:"
                                                                               : L"embedded:";
    return std::wstring(prefix) + std::wstring(entry.raw);
}

void AddUnique(std::vector<Candidate>& output, std::unordered_set<std::wstring>& seen,
               std::wstring text, SemanticLabel label = SemanticLabel::None,
               bool original = false, double score = 0.0, bool isProtected = false,
               std::uint32_t policyFlags = CandidatePolicyNone,
               std::uint32_t sourceFlags = CandidateSourceNone,
               std::wstring dictionaryEntryId = {},
               std::uint8_t socialRange = SocialRangeUnspecified) {
    if (text.empty() || !seen.insert(text).second) return;
    output.push_back({std::move(text), label, original, score, isProtected, policyFlags,
                      sourceFlags, std::move(dictionaryEntryId), socialRange});
}

void AddOriginal(std::vector<Candidate>& output, std::unordered_set<std::wstring>& seen,
                 std::wstring_view raw, SemanticLabel label = SemanticLabel::Original,
                 double score = 0.0, bool isProtected = false,
                 std::uint32_t policyFlags = CandidatePolicyNone,
                 std::uint32_t sourceFlags = CandidateSourceNone,
                 std::wstring dictionaryEntryId = {},
                 std::uint8_t socialRange = SocialRangeUnspecified) {
    AddUnique(output, seen, std::wstring(raw), label, true, score, isProtected, policyFlags,
              sourceFlags, std::move(dictionaryEntryId), socialRange);
}

// Marks the typed text as intended: protected, and first in the list.
void ProtectOriginal(std::vector<Candidate>& output, std::unordered_set<std::wstring>& seen,
                     std::wstring_view raw) {
    auto original = std::find_if(output.begin(), output.end(),
                                 [](const Candidate& candidate) { return candidate.isOriginal; });
    if (original == output.end()) {
        AddOriginal(output, seen, raw);
        original = output.end() - 1;
    }
    original->isProtected = true;
    original->policyFlags |= CandidatePolicyProtect;
    std::rotate(output.begin(), original, original + 1);
}

void FinalizeCandidates(std::vector<Candidate>& candidates, std::wstring_view rawText) {
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        candidates[index].id = static_cast<std::uint32_t>(index + 1);
        candidates[index].replaceSpan = {0, static_cast<std::uint32_t>(rawText.size())};
    }
}

bool HasOriginal(const std::vector<Candidate>& candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [](const Candidate& candidate) { return candidate.isOriginal; });
}

std::vector<std::wstring> SearchPrefixes(std::wstring_view rawText) {
    std::vector<std::wstring> prefixes;
    std::unordered_set<std::wstring> seen;
    const auto addPrefix = [&](std::wstring_view value) {
        if (value.empty()) return;
        std::wstring prefix(value.substr(0, std::min<std::size_t>(3, value.size())));
        if (seen.insert(prefix).second) prefixes.push_back(std::move(prefix));
    };

    addPrefix(rawText);
    if (rawText.size() > 1) {
        for (std::size_t index = 0; index < rawText.size(); ++index) {
            std::wstring deleted(rawText);
            deleted.erase(index, 1);
            addPrefix(deleted);
        }

        std::wstring transposed(rawText);
        std::swap(transposed[0], transposed[1]);
        addPrefix(transposed);
    }
    if (rawText.size() >= 3) {
        for (wchar_t first = L'a'; first <= L'z'; ++first) {
            std::wstring replaced(rawText);
            replaced[0] = first;
            addPrefix(replaced);

            std::wstring inserted(1, first);
            inserted.append(rawText);
            addPrefix(inserted);
        }
    }
    return prefixes;
}

const IMisspellingProvider& DefaultMisspellingProvider() {
    static const NullMisspellingProvider provider;
    return provider;
}

const IFrequencyProvider& DefaultFrequencyProvider() {
    static const NullFrequencyProvider provider;
    return provider;
}

const IPhraseContextProvider& DefaultPhraseContextProvider() {
    static const NullPhraseContextProvider provider;
    return provider;
}

const IUserLearningProvider& DefaultUserLearningProvider() {
    static const NullUserLearningProvider provider;
    return provider;
}

const SocialLearningProvider& DefaultSocialLearningProvider() {
    static const SocialLearningStore store;
    static const SocialLearningProvider provider(store);
    return provider;
}

}  // namespace

CandidateGenerator::CandidateGenerator(const ILexiconProvider& lexiconProvider)
    : CandidateGenerator(lexiconProvider, DefaultMisspellingProvider(),
                         DefaultFrequencyProvider(), DefaultPhraseContextProvider(),
                         DefaultUserLearningProvider()) {}

CandidateGenerator::CandidateGenerator(const ILexiconProvider& lexiconProvider,
                                       const IMisspellingProvider& misspellingProvider)
    : CandidateGenerator(lexiconProvider, misspellingProvider, DefaultFrequencyProvider(),
                         DefaultPhraseContextProvider(), DefaultUserLearningProvider()) {}

CandidateGenerator::CandidateGenerator(const ILexiconProvider& lexiconProvider,
                                       const IFrequencyProvider& frequencyProvider,
                                       const IPhraseContextProvider& phraseProvider)
    : CandidateGenerator(lexiconProvider, DefaultMisspellingProvider(), frequencyProvider,
                         phraseProvider, DefaultUserLearningProvider()) {}

CandidateGenerator::CandidateGenerator(const ILexiconProvider& lexiconProvider,
                                       const IMisspellingProvider& misspellingProvider,
                                       const IFrequencyProvider& frequencyProvider,
                                       const IPhraseContextProvider& phraseProvider,
                                       const IUserLearningProvider& learningProvider)
    : lexiconProvider_(lexiconProvider),
      misspellingProvider_(misspellingProvider),
      frequencyProvider_(frequencyProvider),
      phraseProvider_(phraseProvider),
      learningProvider_(learningProvider) {}

CandidateGenerator::CandidateGenerator(const ILexiconProvider& lexiconProvider,
                                       const ILexiconProvider& supplementaryProvider,
                                       const IMisspellingProvider& misspellingProvider,
                                       const IFrequencyProvider& frequencyProvider,
                                       const IPhraseContextProvider& phraseProvider,
                                       const IUserLearningProvider& learningProvider)
    : CandidateGenerator(lexiconProvider, supplementaryProvider, misspellingProvider,
                         frequencyProvider, phraseProvider, learningProvider,
                         DefaultSocialLearningProvider()) {}

CandidateGenerator::CandidateGenerator(const ILexiconProvider& lexiconProvider,
                                       const ILexiconProvider& supplementaryProvider,
                                       const IMisspellingProvider& misspellingProvider,
                                       const IFrequencyProvider& frequencyProvider,
                                       const IPhraseContextProvider& phraseProvider,
                                       const IUserLearningProvider& learningProvider,
                                       const SocialLearningProvider& socialLearningProvider)
    : lexiconProvider_(lexiconProvider),
      supplementaryProvider_(&supplementaryProvider),
      misspellingProvider_(misspellingProvider),
      frequencyProvider_(frequencyProvider),
      phraseProvider_(phraseProvider),
      learningProvider_(learningProvider),
      socialLearningProvider_(&socialLearningProvider) {}

std::uint32_t CandidateGenerator::Offered(std::wstring_view lowerRaw, std::wstring_view candidate,
                                          std::uint32_t flags) const {
    constexpr std::uint32_t kAutomatic = CandidatePolicyCorrect | CandidatePolicyNormalize;
    if ((flags & kAutomatic) == 0 || !learningProvider_.Undone(lowerRaw, Lower(candidate))) return flags;
    return (flags & ~kAutomatic) | CandidatePolicySuggestOnly;
}

std::vector<Candidate> CandidateGenerator::Generate(std::wstring_view rawText,
                                                    std::wstring_view context,
                                                    std::wstring_view followingContext,
                                                    const ConversionOptions& options) const {
    std::vector<Candidate> output;
    std::unordered_set<std::wstring> seen;
    const auto lower = Lower(rawText);
    const auto previousWords = ContextWords(context);
    const auto followingWords = ContextWords(followingContext);
    const auto contextScore = [&](std::wstring_view word) {
        return ContextScore(phraseProvider_, word, previousWords, followingWords);
    };
    const auto rawContextScore = options.contextSuggestionsEnabled
                                     ? contextScore(lower)
                                     : 0.0;

    if (rawText.empty()) return output;

    if (IsProtectedToken(rawText) && !HasTwoInitialCapitals(rawText)) {
        AddOriginal(output, seen, rawText, SemanticLabel::ProperNoun, 100.0, true,
                    CandidatePolicyProtect | CandidatePolicySuggestOnly,
                    CandidateSourceProtectedPattern);
    }

    bool exact = false;
    bool exactHasWeakFrequency = false;
    // The user keeps this word as typed (user dictionary): nothing replaces it.
    bool keptAsTyped = false;
    lexiconProvider_.Find({LexiconQuery::Kind::Exact, rawText}, [&](const auto& entry) {
        exact = true;
        keptAsTyped = keptAsTyped || (policyEngine_.RawFlags(entry) & CandidatePolicyProtect) != 0;
        const auto frequency = frequencyProvider_.Score(entry.raw);
        exactHasWeakFrequency = exactHasWeakFrequency || (frequency > 0.0 && frequency < 1.0);
        if (entry.originalFirst) {
            const bool isProtected = PolicyEngine::IsTekitoOwnedExpression(entry.rawLabel);
            AddOriginal(output, seen, rawText, entry.rawLabel, 100.0, isProtected,
                        policyEngine_.RawFlags(entry), SourceFlagsFor(entry, entry.rawLabel),
                        DictionaryEntryIdFor(entry, entry.rawLabel));
        }
        AddUnique(output, seen, std::wstring(entry.candidate), entry.candidateLabel, false, 95.0,
                  false, Offered(lower, entry.candidate, policyEngine_.CandidateFlags(entry)),
                  SourceFlagsFor(entry, entry.candidateLabel),
                  DictionaryEntryIdFor(entry, entry.candidateLabel));
        if (!entry.originalFirst) {
            AddOriginal(output, seen, rawText, entry.rawLabel, 90.0, false,
                        policyEngine_.RawFlags(entry), SourceFlagsFor(entry, entry.rawLabel),
                        DictionaryEntryIdFor(entry, entry.rawLabel));
        }
    });
    if (!exactHasWeakFrequency) {
        const auto rawFrequency = frequencyProvider_.Score(rawText);
        exactHasWeakFrequency = rawFrequency > 0.0 && rawFrequency < 1.0;
    }
    if (exactHasWeakFrequency && !keptAsTyped) {
        // A rare exact lexicon entry may be an obsolete/false-positive spelling.
        // Re-run the general typo search; no word-specific exception is used.
        output.clear();
        seen.clear();
    }

    const auto addCompletions = [&]() {
        lexiconProvider_.Find({LexiconQuery::Kind::Prefix, rawText, 9}, [&](const auto& entry) {
            const auto comparableRaw = entry.caseSensitive ? std::wstring(entry.raw) : Lower(entry.raw);
            const auto comparableInput = entry.caseSensitive ? std::wstring(rawText) : lower;
            if (!entry.spellcheckWord || !comparableRaw.starts_with(comparableInput) ||
                comparableRaw.size() <= comparableInput.size()) {
                return;
            }
            AddUnique(output, seen, MatchCase(rawText, std::wstring(entry.raw)), SemanticLabel::None, false,
                      80.0, false, CandidatePolicySuggestOnly,
                      SourceFlagsFor(entry, SemanticLabel::Original) | CandidateSourceCompletion,
                      DictionaryEntryIdFor(entry, SemanticLabel::Original));
            if (!HasOriginal(output)) AddOriginal(output, seen, rawText);
            if (output.size() >= 9) return;
        });
    };

    if (options.completionEnabled && lower.size() <= 2 && !exact) addCompletions();

    std::vector<CorrectionCandidate> ranked;
    if (options.correctionEnabled && options.commonMisspellingsEnabled &&
        !IsProtectedToken(rawText)) {
        misspellingProvider_.Find(lower, [&](const auto& misspelling) {
            const auto correction = Lower(misspelling.correction);
            if (correction.empty() || correction == lower) return;

            const auto evidence = typoModel_.Compare(lower, correction);
            LexiconEntry entry{misspelling.correction, {}, SemanticLabel::Original,
                               SemanticLabel::None, true, true};
            entry.sourceFlags = CandidateSourceWikipediaMisspelling;
            ranked.push_back({evidence.distance,
                              evidence.repeatedKey,
                              evidence.transposition,
                              evidence.missingCharacter,
                              evidence.extraCharacter,
                              evidence.qwertyNeighbor,
                              entry,
                              std::wstring(misspelling.correction),
                              {frequencyProvider_.Score(misspelling.correction),
                               options.contextSuggestionsEnabled
                                   ? phraseProvider_.Score(misspelling.correction, context)
                                   : 0.0,
                               learningProvider_.Score(lower, Lower(misspelling.correction)),
                               options.contextSuggestionsEnabled
                                   ? contextScore(misspelling.correction) - rawContextScore
                                   : 0.0},
                              true, false, exact && !exactHasWeakFrequency,
                              typoModel_.WeightedCost(lower, correction)});
        });
    }

    std::unordered_set<std::wstring> inspected;
    const auto addRankedEntry = [&](const LexiconEntry& entry, bool weakExactRecovery = false) {
        if (!options.correctionEnabled || !entry.spellcheckWord || entry.caseSensitive) return;
        if (!inspected.insert(std::wstring(entry.raw)).second) return;
        const auto comparableRaw = Lower(entry.raw);
        const auto evidence = typoModel_.Compare(lower, comparableRaw);
        const auto distance = evidence.distance;
        if (distance > 0 && distance <= 2) {
            const auto candidateContextScore = options.contextSuggestionsEnabled
                                                    ? contextScore(entry.raw)
                                                    : 0.0;
            ranked.push_back({distance, evidence.repeatedKey,
                              evidence.transposition, evidence.missingCharacter,
                              evidence.extraCharacter, evidence.qwertyNeighbor, entry,
                              std::wstring(entry.raw),
                              {frequencyProvider_.Score(entry.raw),
                               candidateContextScore,
                               learningProvider_.Score(lower, Lower(entry.raw)),
                               candidateContextScore - rawContextScore},
                              false, weakExactRecovery, exact && !exactHasWeakFrequency,
                              typoModel_.WeightedCost(lower, comparableRaw)});
        }
    };
    const auto candidateSearchLimit = lower.size() <= 2 ? 4096u : 512u;
    if (exactHasWeakFrequency) {
        lexiconProvider_.Find({LexiconQuery::Kind::Candidate, lower, candidateSearchLimit},
                              [&](const auto& entry) { addRankedEntry(entry, true); });
    } else {
        lexiconProvider_.Find({LexiconQuery::Kind::Candidate, lower, candidateSearchLimit},
                              addRankedEntry);
    }
    for (const auto& prefix : SearchPrefixes(lower)) {
        // Bounded index ranges keep the time per key predictable. Widen this
        // only after measuring missed candidates on the real packs.
        lexiconProvider_.Find({LexiconQuery::Kind::Prefix, prefix, 512},
                              addRankedEntry);
    }
    ranker_.RankCorrections(ranked, lower.size());

    // A word typed right stays first: a word a letter away that fits the
    // context better is only offered. Deciding it from the words before
    // alone rewrote about one word in sixty of real text ("buy them," to
    // "buy the"), far more often than it caught a slip.
    if (exact && std::any_of(ranked.begin(), ranked.end(), [](const auto& correction) {
            return correction.validWordConflict;
        })) {
        AddOriginal(output, seen, rawText);
    }

    bool addedFirstCorrection = false;
    std::size_t addedCorrectionCount = 0;
    for (const auto& correction : ranked) {
        const bool hasTypoShape = correction.repeatedKey || correction.transposition ||
                                  correction.missingCharacter || correction.extraCharacter;
        const bool sameLength = correction.rawText.size() == rawText.size();
        if (!correction.sourceBackedCorrection && correction.distance > 1 &&
            !hasTypoShape && sameLength) {
            continue;
        }

        auto entry = correction.entry;
        if (!correction.rawText.empty()) entry.raw = correction.rawText;
        auto sourceFlags = SourceFlagsFor(entry, SemanticLabel::Original);
        if (!correction.sourceBackedCorrection) {
            sourceFlags |= CandidateSourceGeneratedEditDistance |
                           (correction.distance == 1 ? CandidateSourceSingleEdit
                                                     : CandidateSourceNone) |
                           (correction.repeatedKey ? CandidateSourceRepeatedKey : CandidateSourceNone) |
                           (correction.transposition ? CandidateSourceTransposition : CandidateSourceNone) |
                           (correction.missingCharacter ? CandidateSourceMissingCharacter : CandidateSourceNone) |
                           (correction.extraCharacter ? CandidateSourceExtraCharacter : CandidateSourceNone) |
                           (correction.qwertyNeighbor ? CandidateSourceQwertyNeighbor : CandidateSourceNone);
        }
        const auto policy = correction.validWordConflict || keptAsTyped
                                ? CandidatePolicySuggestOnly | CandidatePolicyContext
                                : Offered(lower, entry.raw, CandidatePolicyCorrect);
        const auto sizeBefore = output.size();
        AddUnique(output, seen, MatchCase(rawText, std::wstring(entry.raw)), SemanticLabel::None, false,
                  100.0 - static_cast<double>(correction.distance), false, policy,
                  sourceFlags, DictionaryEntryIdFor(entry, SemanticLabel::Original));
        if (output.size() != sizeBefore) ++addedCorrectionCount;
        if (!addedFirstCorrection) {
            addedFirstCorrection = true;
            if (!HasOriginal(output)) AddOriginal(output, seen, rawText);
        }
        if (addedCorrectionCount >= 64) break;
    }

    if (options.completionEnabled && (lower.size() > 2 || exact)) addCompletions();

    if (supplementaryProvider_) {
        const auto supplementaryStart = output.size();
        std::size_t socialCandidateCount = 0;
        std::size_t socialTextCount = 0;
        std::size_t socialEmojiCount = 0;
        const auto socialCandidateLimit = options.socialExpressionRange <= 1 ? 4u :
                                          options.socialExpressionRange == 2 ? 6u :
                                          options.socialExpressionRange == 3 ? 8u : 12u;
        const bool explicitSocial = rawText.size() > 1 && rawText.front() == L':';
        const auto socialQuery = explicitSocial ? rawText.substr(1) : rawText;
        const bool casualInput = explicitSocial || !exact;
        std::vector<std::wstring> curatedMeanings;
        std::size_t meaningIndex = 1;  // right after the typed word, or after its emoji
        const auto emitSupplementary = [&](const auto& entry, std::wstring_view queryText) {
            // TEKITO's own slang and expression lists are curated: a spelling
            // listed there ("brb", "omw") is meant, so it stays first and is
            // never auto-corrected, whether or not its suggestions are shown.
            // Larger external lists (Wiktionary, place names) also contain
            // common typos and only add suggestions.
            constexpr auto kCuratedSources =
                CandidateSourceTekitoOwnedSlang | CandidateSourceSocialExpression;
            if (entry.originalFirst && (entry.sourceFlags & kCuratedSources) != 0 &&
                (policyEngine_.RawFlags(entry) & CandidatePolicyProtect) != 0) {
                ProtectOriginal(output, seen, rawText);
                if (entry.candidateLabel != SemanticLabel::Emoji) {
                    curatedMeanings.emplace_back(entry.candidate);
                }
            }
            const bool social = entry.socialRange != SocialRangeUnspecified;
            const bool phonetic = (entry.sourceFlags & CandidateSourceJapanesePhonetic) != 0;
            const bool emoji = entry.candidateLabel == SemanticLabel::Emoji ||
                               (entry.sourceFlags & CandidateSourceEmoji) != 0;
            if (social && options.socialExpressionRange <= 0) return;
            if (social && !emoji && entry.socialRange > options.socialExpressionRange) return;
            if (phonetic && !options.japanesePhoneticSuggestionsEnabled) return;
            if (social && !explicitSocial) {
                if (socialCandidateCount >= socialCandidateLimit) return;
                const auto& categoryCount = emoji ? socialEmojiCount : socialTextCount;
                const auto categoryLimit = options.socialExpressionRange <= 1 ? 2u :
                                            options.socialExpressionRange == 2 ? 3u :
                                            options.socialExpressionRange == 3 ? 4u : 6u;
                if (categoryCount >= categoryLimit) return;
            }
            if (entry.originalFirst) {
                AddOriginal(output, seen, rawText, entry.rawLabel, 70.0,
                            (policyEngine_.RawFlags(entry) & CandidatePolicyProtect) != 0,
                            policyEngine_.RawFlags(entry),
                            SourceFlagsFor(entry, entry.rawLabel),
                            DictionaryEntryIdFor(entry, entry.rawLabel), entry.socialRange);
            }
            const auto before = output.size();
            AddUnique(output, seen, std::wstring(entry.candidate), entry.candidateLabel,
                      false, 65.0 + (emoji && casualInput ? 8.0 : 0.0) +
                                 (social && socialLearningProvider_
                                      ? socialLearningProvider_->Score(queryText, entry.candidate) * 10.0
                                      : 0.0),
                      false, policyEngine_.CandidateFlags(entry),
                      SourceFlagsFor(entry, entry.candidateLabel),
                      DictionaryEntryIdFor(entry, entry.candidateLabel), entry.socialRange);
            if (social && output.size() != before) {
                ++socialCandidateCount;
                if (entry.candidateLabel == SemanticLabel::Emoji) {
                    ++socialEmojiCount;
                } else {
                    ++socialTextCount;
                }
            }
        };
        supplementaryProvider_->Find({LexiconQuery::Kind::Exact, rawText, 32},
                                     [&](const auto& entry) { emitSupplementary(entry, rawText); });
        if (explicitSocial) {
            supplementaryProvider_->Find({LexiconQuery::Kind::Exact, socialQuery, 32},
                                         [&](const auto& entry) { emitSupplementary(entry, socialQuery); });
        }
        if (options.completionEnabled) {
            supplementaryProvider_->Find({LexiconQuery::Kind::Prefix, rawText, 16},
                                         [&](const auto& entry) {
                if (entry.socialRange != SocialRangeUnspecified ||
                    ((entry.sourceFlags & CandidateSourceJapanesePhonetic) != 0 &&
                     !options.japanesePhoneticSuggestionsEnabled)) return;
                const auto entryRaw = Lower(entry.raw);
                if (entryRaw.size() <= lower.size()) return;
                AddUnique(output, seen, MatchCase(rawText, std::wstring(entry.raw)),
                          SemanticLabel::None, false, 60.0, false,
                          CandidatePolicySuggestOnly,
                          SourceFlagsFor(entry, SemanticLabel::Original) |
                              CandidateSourceCompletion,
                          DictionaryEntryIdFor(entry, SemanticLabel::Original));
            });
        }

        std::vector<std::size_t> socialPositions;
        for (std::size_t index = supplementaryStart; index < output.size(); ++index) {
            if (!output[index].isOriginal && output[index].socialRange != SocialRangeUnspecified) {
                socialPositions.push_back(index);
            }
        }
        std::stable_sort(socialPositions.begin(), socialPositions.end(), [&](std::size_t left,
                                                                                std::size_t right) {
            return output[left].score > output[right].score;
        });
        std::vector<Candidate> sortedSocial;
        sortedSocial.reserve(socialPositions.size());
        for (const auto index : socialPositions) sortedSocial.push_back(output[index]);
        for (std::size_t index = 0; index < socialPositions.size(); ++index) {
            output[socialPositions[index]] = std::move(sortedSocial[index]);
        }

        if (!socialPositions.empty() && !explicitSocial) {
            std::vector<Candidate> promotedEmoji;
            for (std::size_t index = output.size(); index-- > supplementaryStart;) {
                if (output[index].label == SemanticLabel::Emoji) {
                    promotedEmoji.push_back(std::move(output[index]));
                    output.erase(output.begin() + static_cast<std::ptrdiff_t>(index));
                }
            }
            if (!promotedEmoji.empty()) {
                const auto pageOneIndex = casualInput ? 1u : 4u;
                const auto insertion = std::min<std::size_t>(pageOneIndex, output.size());
                output.insert(output.begin() + static_cast<std::ptrdiff_t>(insertion),
                              std::make_move_iterator(promotedEmoji.rbegin()),
                              std::make_move_iterator(promotedEmoji.rend()));
                meaningIndex = insertion + promotedEmoji.size();
            }
        }

        // What a curated abbreviation stands for ("brb" -> "be right back")
        // comes right after it and its emoji, ahead of spelling look-alikes.
        const auto meaningSlot =
            output.begin() + static_cast<std::ptrdiff_t>(std::min(meaningIndex, output.size()));
        for (auto meaning = curatedMeanings.rbegin(); meaning != curatedMeanings.rend(); ++meaning) {
            const auto found = std::find_if(meaningSlot, output.end(),
                                            [&](const Candidate& candidate) {
                                                return !candidate.isOriginal &&
                                                       candidate.text == *meaning;
                                            });
            if (found != output.end()) std::rotate(meaningSlot, found, found + 1);
        }
    }

    if (!HasOriginal(output)) AddOriginal(output, seen, rawText);
    ApplyWritingRules(output, seen, rawText, lower, exact && !exactHasWeakFrequency, options);
    ApplyDoubledWord(output, rawText, lower, context, options);
    // A word in quotes ('hi', 'hi') or a plural's apostrophe (students'):
    // not a slip, so nothing replaces it on its own.
    const auto isQuote = [](wchar_t ch) { return ch == L'\'' || ch == L'\u2019' || ch == L'\u2018'; };
    std::wstring_view inside(lower);
    while (!inside.empty() && isQuote(inside.front())) inside.remove_prefix(1);
    while (!inside.empty() && isQuote(inside.back())) inside.remove_suffix(1);
    if (!inside.empty() && inside.size() != lower.size() && IsWord(inside)) {
        for (auto& candidate : output) {
            if (!candidate.isOriginal &&
                (candidate.policyFlags & (CandidatePolicyCorrect | CandidatePolicyNormalize)) != 0) {
                candidate.policyFlags = (candidate.policyFlags & ~(CandidatePolicyCorrect | CandidatePolicyNormalize)) |
                                        CandidatePolicySuggestOnly;
            }
        }
    }
    AddPredictions(output, seen, rawText, lower, context, options);
    FinalizeCandidates(output, rawText);
    return output;
}

bool CandidateGenerator::IsWord(std::wstring_view lower) const {
    bool found = false;
    lexiconProvider_.Find({LexiconQuery::Kind::Exact, lower}, [&](const auto& entry) {
        found = found || entry.spellcheckWord;
    });
    return found;
}

void CandidateGenerator::ApplyWritingRules(std::vector<Candidate>& output, std::unordered_set<std::wstring>& seen,
                                           std::wstring_view rawText, std::wstring_view lower, bool exact,
                                           const ConversionOptions& options) const {
    // A word the user keeps as typed, or a name or code they capitalized,
    // stays as it is.
    const auto original = std::find_if(output.begin(), output.end(),
                                       [](const Candidate& candidate) { return candidate.isOriginal; });
    if (original != output.end() && original->isProtected) return;
    if (std::any_of(rawText.begin(), rawText.end(), [](wchar_t ch) { return std::iswdigit(ch) != 0; })) return;

    struct Rule {
        std::wstring text;
        bool automatic;
    };
    std::optional<Rule> first;   // replaces the word on Space
    std::vector<std::wstring> offered;  // right after the word as typed
    const bool typedSmall = IsAllLower(rawText);

    if (HasTwoInitialCapitals(rawText)) {
        // "THe": one capital was meant, when the word is a common one.
        const bool common = rawText.size() > 3 || frequencyProvider_.Score(lower) >= 5.0;
        if (IsWord(lower) && common) {
            std::wstring text = Lower(rawText);
            text.front() = static_cast<wchar_t>(std::towupper(text.front()));
            first = Rule{std::move(text), true};
        }
    } else if (rawText == L"i") {
        first = Rule{L"I", true};
    } else if (typedSmall) {
        if (const auto written = WrittenForm(lower)) {
            if (written->second) {
                first = Rule{written->first, true};
            } else {
                offered.push_back(written->first);
            }
        }
    }

    // A contraction without its apostrophe (dont, youre, theyre).
    if (!first && lower.find(L'\'') == std::wstring_view::npos && lower.size() >= 2) {
        constexpr std::wstring_view kEndings[] = {L"n't", L"'re", L"'ve", L"'ll", L"'d", L"'s", L"'m"};
        std::optional<std::wstring> contraction;
        for (std::size_t at = 1; at < lower.size() && !contraction; ++at) {
            std::wstring text = std::wstring(lower.substr(0, at)) + L"'" + std::wstring(lower.substr(at));
            const bool shaped = text == L"y'all" || std::any_of(std::begin(kEndings), std::end(kEndings), [&](auto ending) {
                return text.ends_with(ending) && text.size() - ending.size() + ending.find(L'\'') == at;
            });
            if (shaped && IsWord(text)) contraction = std::move(text);
        }
        if (contraction) {
            if (contraction->starts_with(L"i'")) contraction->front() = L'I';
            auto text = WithCaseOf(rawText, *contraction);
            // Already put first by the word lists ("im" to "I'm"): kept so.
            const bool listed = !output.empty() && !output.front().isOriginal && output.front().text == text &&
                                (output.front().policyFlags & (CandidatePolicyCorrect | CandidatePolicyNormalize)) != 0;
            // "'s" after a name or noun is a possessive the user may not mean
            // (heros, churchs): only offered.
            constexpr std::wstring_view kIsWords[] = {L"he's", L"she's", L"it's", L"that's", L"what's", L"where's",
                                                      L"who's", L"there's", L"here's", L"how's", L"when's", L"why's",
                                                      L"let's", L"everyone's", L"someone's", L"nobody's",
                                                      L"everything's", L"something's", L"nothing's"};
            const bool possessive = contraction->ends_with(L"'s") &&
                                    std::find(std::begin(kIsWords), std::end(kIsWords), *contraction) == std::end(kIsWords);
            if (listed || (!exact && !possessive)) {
                first = Rule{std::move(text), true};
            } else if (!exact) {
                offered.push_back(std::move(text));
            } else if (frequencyProvider_.Score(*contraction) >= frequencyProvider_.Score(lower) + 0.7) {
                // A word of its own too (cant, wont): kept, and the
                // contraction offered first.
                offered.push_back(std::move(text));
            }
        }
    }

    // Two words typed without the space between them (ofthe, alot).
    if (!first && !exact && typedSmall && lower.size() >= 4 &&
        std::all_of(lower.begin(), lower.end(), [](wchar_t ch) { return std::iswalpha(ch) != 0; })) {
        double best = 0.0;
        std::wstring split;
        // "a" run into the next word, where that is the usual phrase.
        constexpr std::pair<std::wstring_view, std::wstring_view> kPhrases[] = {
            {L"alot", L"a lot"}, {L"abit", L"a bit"}, {L"alittle", L"a little"}, {L"afew", L"a few"}};
        for (const auto& [typed, phrase] : kPhrases) {
            if (lower == typed) {
                split = std::wstring(phrase);
                best = 9.0;
            }
        }
        for (std::size_t at = 1; at + 1 < lower.size(); ++at) {
            const auto left = lower.substr(0, at);
            const auto right = lower.substr(at);
            if (at == 1) continue;
            // Two-letter halves only among the everyday words (not "st", "co").
            constexpr std::wstring_view kShortWords[] = {L"of", L"in", L"on", L"at", L"to", L"is", L"it", L"be", L"by",
                                                         L"as", L"an", L"or", L"if", L"so", L"no", L"do", L"go", L"he",
                                                         L"me", L"my", L"we", L"up", L"us", L"am"};
            const auto everyday = [&](std::wstring_view half) {
                return half.size() != 2 || std::find(std::begin(kShortWords), std::end(kShortWords), half) != std::end(kShortWords);
            };
            if (!everyday(left) || !everyday(right)) continue;
            const double score = std::min<double>(frequencyProvider_.Score(left), frequencyProvider_.Score(right));
            if (score > best && IsWord(left) && IsWord(right)) {
                best = score;
                split = (left == L"i" ? std::wstring(L"I") : std::wstring(left)) + L" " + std::wstring(right);
            }
        }
        // Both halves common words; applied as below, otherwise offered.
        constexpr double kCommonWord = 4.0;
        if (!split.empty() && best >= kCommonWord) {
            const auto correction = std::find_if(output.begin(), output.end(), [](const Candidate& candidate) {
                return !candidate.isOriginal && (candidate.policyFlags & CandidatePolicyCorrect) != 0;
            });
            // A closest word that is one of the halves would drop the other.
            const auto closestText = correction == output.end() ? std::wstring() : Lower(correction->text);
            const bool dropsWord = !closestText.empty() && (split.ends_with(L" " + closestText) ||
                                                            split.starts_with(closestText + L" "));
            // Otherwise the closest word is as likely meant (powerfull is
            // powerful, not "power full"), or the word is a compound the
            // lexicon lacks (offseason, wellbeing): the split is only offered.
            if (dropsWord || best >= 9.0) {
                first = Rule{std::move(split), true};
            } else {
                offered.push_back(std::move(split));
            }
        }
    }

    if (!first && offered.empty()) return;
    const auto automatic = [&](const std::wstring& text) {
        return options.correctionEnabled ? Offered(lower, Lower(text), CandidatePolicyNormalize)
                                         : CandidatePolicySuggestOnly;
    };
    auto originalAt = [&] {
        return std::find_if(output.begin(), output.end(), [](const Candidate& candidate) { return candidate.isOriginal; });
    };
    for (auto it = offered.rbegin(); it != offered.rend(); ++it) {
        auto candidate = TakeOut(output, *it);
        seen.insert(*it);
        candidate.policyFlags = CandidatePolicySuggestOnly;
        const auto at = originalAt();
        output.insert(at == output.end() ? output.begin() : at + 1, std::move(candidate));
    }
    if (first) {
        auto candidate = TakeOut(output, first->text);
        seen.insert(first->text);
        // Only this one is applied: the rest are offered.
        for (auto& other : output) {
            if (!other.isOriginal && (other.policyFlags & (CandidatePolicyCorrect | CandidatePolicyNormalize)) != 0) {
                other.policyFlags = (other.policyFlags & ~(CandidatePolicyCorrect | CandidatePolicyNormalize)) |
                                        CandidatePolicySuggestOnly;
            }
        }
        candidate.policyFlags = automatic(first->text);
        candidate.sourceFlags |= CandidateSourceSingleEdit;
        if (candidate.label == SemanticLabel::ProperNoun) candidate.label = SemanticLabel::None;
        output.insert(output.begin(), std::move(candidate));
    }
}

void CandidateGenerator::ApplyDoubledWord(std::vector<Candidate>& output, std::wstring_view rawText,
                                          std::wstring_view lower, std::wstring_view context,
                                          const ConversionOptions& options) const {
    if (!options.doubledWords || lower.empty() ||
        !std::all_of(lower.begin(), lower.end(), [](wchar_t ch) { return std::iswalpha(ch) != 0; })) {
        return;
    }
    const auto original = std::find_if(output.begin(), output.end(),
                                       [](const Candidate& candidate) { return candidate.isOriginal; });
    if (original != output.end() && original->isProtected) return;
    // The word before, with only spaces between.
    auto end = context.size();
    while (end > 0 && (context[end - 1] == L' ' || context[end - 1] == L'\t')) --end;
    if (end == context.size()) return;
    auto start = end;
    while (start > 0 && (std::iswalpha(context[start - 1]) || context[start - 1] == L'\'')) --start;
    if (start == end || Lower(context.substr(start, end - start)) != lower) return;

    // Never meant twice; others sometimes are ("that that", "had had").
    constexpr std::wstring_view kNeverTwice[] = {L"the", L"a",    L"an",  L"of",    L"and", L"for",
                                                 L"with", L"from", L"by", L"my",    L"your", L"our",
                                                 L"their", L"its", L"to"};
    const bool automatic = options.correctionEnabled &&
                           std::find(std::begin(kNeverTwice), std::end(kNeverTwice), lower) != std::end(kNeverTwice);
    Candidate removal;
    removal.score = 100.0;
    removal.sourceFlags = CandidateSourceDoubledWord;
    removal.policyFlags = automatic ? Offered(lower, L"", CandidatePolicyNormalize) : CandidatePolicySuggestOnly;
    if ((removal.policyFlags & CandidatePolicyNormalize) == 0) {
        const auto at = std::find_if(output.begin(), output.end(),
                                     [](const Candidate& candidate) { return candidate.isOriginal; });
        output.insert(at == output.end() ? output.end() : at + 1, std::move(removal));
        return;
    }
    for (auto& other : output) {
        if (!other.isOriginal && (other.policyFlags & (CandidatePolicyCorrect | CandidatePolicyNormalize)) != 0) {
            other.policyFlags = (other.policyFlags & ~(CandidatePolicyCorrect | CandidatePolicyNormalize)) |
                                CandidatePolicySuggestOnly;
        }
    }
    output.insert(output.begin(), std::move(removal));
    (void)rawText;
}

void CandidateGenerator::AddPredictions(std::vector<Candidate>& output, std::unordered_set<std::wstring>& seen,
                                        std::wstring_view rawText, std::wstring_view lower,
                                        std::wstring_view context, const ConversionOptions& options) const {
    if (!options.nextWordPrediction || lower.empty() ||
        !std::all_of(lower.begin(), lower.end(), [](wchar_t ch) { return std::iswalpha(ch) != 0; })) {
        return;
    }
    // Only the words of this sentence say what comes next.
    const auto sentenceEnd = context.find_last_of(L".!?\r\n");
    const auto words = ContextWords(sentenceEnd == std::wstring_view::npos ? context : context.substr(sentenceEnd + 1));
    if (words.empty()) return;

    constexpr std::size_t kPredictions = 3;
    std::vector<std::wstring> predicted;
    const auto collect = [&](const std::wstring& key) {
        for (auto& word : phraseProvider_.Following(key, lower, 8)) {
            if (predicted.size() >= kPredictions) return;
            if (std::find(predicted.begin(), predicted.end(), word) != predicted.end() || !IsWord(word)) continue;
            predicted.push_back(std::move(word));
        }
    };
    if (words.size() >= 2) collect(JoinWords(words, words.size() - 2, 2));
    collect(words.back());
    if (predicted.empty()) return;

    std::vector<Candidate> taken;
    for (const auto& word : predicted) {
        const auto text = MatchCase(rawText, word);
        // Already first (a correction, or the word itself): stays there.
        if (!output.empty() && output.front().text == text) continue;
        auto candidate = TakeOut(output, text);
        seen.insert(text);
        candidate.policyFlags = CandidatePolicySuggestOnly;
        candidate.sourceFlags |= CandidateSourcePrediction;
        taken.push_back(std::move(candidate));
    }
    // After the first candidate and the word as typed.
    std::size_t at = std::min<std::size_t>(1, output.size());
    if (at < output.size() && output[at].isOriginal) ++at;
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(at), std::make_move_iterator(taken.begin()),
                  std::make_move_iterator(taken.end()));
}

}  // namespace tekito
