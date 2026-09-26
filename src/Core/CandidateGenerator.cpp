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
        if (std::iswalpha(ch) || ch == L'\'') {
            current.push_back(static_cast<wchar_t>(std::towlower(ch)));
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

    if (IsProtectedToken(rawText)) {
        AddOriginal(output, seen, rawText, SemanticLabel::ProperNoun, 100.0, true,
                    CandidatePolicyProtect | CandidatePolicySuggestOnly,
                    CandidateSourceProtectedPattern);
    }

    bool exact = false;
    bool exactHasWeakFrequency = false;
    lexiconProvider_.Find({LexiconQuery::Kind::Exact, rawText}, [&](const auto& entry) {
        exact = true;
        const auto frequency = frequencyProvider_.Score(entry.raw);
        exactHasWeakFrequency = exactHasWeakFrequency || (frequency > 0.0 && frequency < 1.0);
        if (entry.originalFirst) {
            const bool isProtected = PolicyEngine::IsTekitoOwnedExpression(entry.rawLabel);
            AddOriginal(output, seen, rawText, entry.rawLabel, 100.0, isProtected,
                        policyEngine_.RawFlags(entry), SourceFlagsFor(entry, entry.rawLabel),
                        DictionaryEntryIdFor(entry, entry.rawLabel));
        }
        AddUnique(output, seen, std::wstring(entry.candidate), entry.candidateLabel, false, 95.0,
                  false, policyEngine_.CandidateFlags(entry),
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
    bool hasStrongContextualAlternative = false;
    if (options.correctionEnabled && exact && !exactHasWeakFrequency &&
        options.contextSuggestionsEnabled && rawText.size() > 2) {
        const auto inspectAlternative = [&](const auto& entry) {
            if (!entry.spellcheckWord || entry.caseSensitive) return;
            const auto evidence = typoModel_.Compare(lower, Lower(entry.raw));
            const auto advantage = contextScore(entry.raw) - rawContextScore;
            hasStrongContextualAlternative =
                hasStrongContextualAlternative ||
                (evidence.distance == 1 && advantage >= kStrongContextConfidence);
        };
        for (const auto& prefix : SearchPrefixes(lower)) {
            if (hasStrongContextualAlternative) break;
            lexiconProvider_.Find({LexiconQuery::Kind::Prefix, prefix, 512},
                                  inspectAlternative);
        }
    }
    if (exactHasWeakFrequency) {
        // A rare exact lexicon entry may be an obsolete/false-positive spelling.
        // Re-run the general typo search; no word-specific exception is used.
        output.clear();
        seen.clear();
    } else if (exact && hasStrongContextualAlternative) {
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

    const bool hasStrongValidCorrection = std::any_of(
        ranked.begin(), ranked.end(), [](const auto& correction) {
            return correction.validWordConflict &&
                   correction.signals.contextConfidence >= kStrongContextConfidence;
        });
    if (exact && std::any_of(ranked.begin(), ranked.end(), [](const auto& correction) {
            return correction.validWordConflict;
        }) && !hasStrongValidCorrection) {
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
        const bool contextAccepted = !correction.validWordConflict ||
                                     correction.signals.contextConfidence >=
                                         kStrongContextConfidence;
        const auto policy = contextAccepted
                                ? CandidatePolicyCorrect
                                : CandidatePolicySuggestOnly | CandidatePolicyContext;
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
        const auto emitSupplementary = [&](const auto& entry, std::wstring_view queryText) {
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
            }
        }
    }

    if (!HasOriginal(output)) AddOriginal(output, seen, rawText);
    FinalizeCandidates(output, rawText);
    return output;
}

}  // namespace tekito
