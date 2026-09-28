#pragma once

#include <cstdint>
#include <string>

namespace tekito {

enum class SemanticLabel {
    None,
    Original,
    ProperNoun,
    Slang,
    Abbreviation,
    Phrase,
    Reaction,
    Standard,
    Emoji,
    // A reading of the keys with a slip undone, or a spelling correction.
    Suggestion,
};

enum CandidatePolicyFlags : std::uint32_t {
    CandidatePolicyNone = 0,
    CandidatePolicyNormalize = 1u << 0,
    CandidatePolicyCorrect = 1u << 1,
    CandidatePolicyProtect = 1u << 2,
    CandidatePolicyExpand = 1u << 3,
    CandidatePolicyContext = 1u << 4,
    CandidatePolicySuggestOnly = 1u << 5,
    CandidatePolicyEmoji = 1u << 6,
};

enum CandidateSourceFlags : std::uint32_t {
    CandidateSourceNone = 0,
    CandidateSourceEmbeddedLexicon = 1u << 0,
    CandidateSourceGeneratedEditDistance = 1u << 1,
    CandidateSourceProtectedPattern = 1u << 2,
    CandidateSourceTekitoOwnedSlang = 1u << 3,
    CandidateSourceEmoji = 1u << 4,
    CandidateSourceRepeatedKey = 1u << 5,
    CandidateSourceTransposition = 1u << 6,
    CandidateSourceMissingCharacter = 1u << 7,
    CandidateSourceExtraCharacter = 1u << 8,
    CandidateSourceCompletion = 1u << 9,
    CandidateSourceUserDictionary = 1u << 10,
    CandidateSourceSingleEdit = 1u << 11,
    CandidateSourceExternalLexicon = 1u << 12,
    CandidateSourceQwertyNeighbor = 1u << 13,
    CandidateSourceWikipediaMisspelling = 1u << 14,
    CandidateSourceProperNoun = 1u << 15,
    CandidateSourceWiktionary = 1u << 16,
    CandidateSourceSocialExpression = 1u << 17,
    CandidateSourceJapanesePhonetic = 1u << 18,
    CandidateSourceQwertyTypoCatalog = 1u << 19,
};

enum SocialRangeTier : std::uint8_t {
    SocialRangeUnspecified = 0,
    SocialRangeCommon = 1,
    SocialRangeFamiliar = 2,
    SocialRangeBroad = 3,
    SocialRangeFull = 4,
};

struct TextSpan {
    std::uint32_t start{0};
    std::uint32_t length{0};

    bool operator==(const TextSpan&) const = default;
};

struct Candidate {
    std::wstring text;
    SemanticLabel label{SemanticLabel::None};
    bool isOriginal{false};
    double score{0.0};
    bool isProtected{false};
    std::uint32_t policyFlags{CandidatePolicyNone};
    std::uint32_t sourceFlags{CandidateSourceNone};
    std::wstring dictionaryEntryId;
    std::uint8_t socialRange{SocialRangeUnspecified};
    std::uint32_t id{0};
    TextSpan replaceSpan;
    // The candidate window marks it with a dictionary sign: there is a
    // meaning to show for it.
    bool hasMeaning{false};

    bool operator==(const Candidate&) const = default;
};

}  // namespace tekito
