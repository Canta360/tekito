#include "Core/CandidateRanker.h"

#include <algorithm>
#include <cmath>

namespace tekito {

namespace {

std::size_t RawSize(const CorrectionCandidate& candidate) {
    return candidate.rawText.empty() ? candidate.entry.raw.size()
                                     : candidate.rawText.size();
}

// Combines the weighted error cost with frequency and context into one
// continuous score. A strict priority chain (integer distance, then length,
// then frequency, ...) always lets a smaller integer distance or a same-
// length candidate win outright regardless of how implausible it otherwise
// is -- a distance-1 rare word beats a distance-2 common word every time.
// Weighing the signals together instead lets a large frequency or context
// gap outweigh a small error-cost gap, and vice versa.
double CombinedScore(const CorrectionCandidate& candidate) {
    constexpr double kErrorWeight = 10.0;
    constexpr double kFrequencyWeight = 1.0;
    // data/phrase's raw scores run roughly 3-50, versus frequency's roughly
    // 0-7 (both are corpus-derived but on different scales), so this must be
    // scaled down to keep context a proportionate vote alongside frequency
    // and error cost rather than one that mechanically dominates both
    // whenever it is nonzero.
    constexpr double kContextWeight = 0.15;
    return -kErrorWeight * candidate.weightedCost +
           kFrequencyWeight * candidate.signals.frequency +
           kContextWeight * candidate.signals.phraseContext;
}

}  // namespace

void CandidateRanker::RankCorrections(std::vector<CorrectionCandidate>& candidates,
                                      std::size_t inputSize) const {
    std::stable_sort(candidates.begin(), candidates.end(), [inputSize](const auto& left, const auto& right) {
        if (left.weakExactRecovery != right.weakExactRecovery) {
            return left.weakExactRecovery;
        }
        if (left.weakExactRecovery || right.weakExactRecovery) {
            if (left.signals.frequency != right.signals.frequency) {
                return left.signals.frequency > right.signals.frequency;
            }
        }
        if (left.validWordConflict && right.validWordConflict) {
            if (left.signals.contextConfidence != right.signals.contextConfidence) {
                return left.signals.contextConfidence > right.signals.contextConfidence;
            }
            if (left.signals.frequency != right.signals.frequency) {
                return left.signals.frequency > right.signals.frequency;
            }
        }
        const auto learningDelta = left.signals.userLearning - right.signals.userLearning;
        constexpr double kLearnedPreferenceThreshold = 0.20;
        constexpr double kLearnedPreferenceMargin = 0.10;
        if (std::abs(learningDelta) >= kLearnedPreferenceMargin &&
            std::max(std::abs(left.signals.userLearning),
                     std::abs(right.signals.userLearning)) >= kLearnedPreferenceThreshold) {
            return learningDelta > 0.0;
        }
        const auto leftScore = CombinedScore(left);
        const auto rightScore = CombinedScore(right);
        if (leftScore != rightScore) return leftScore > rightScore;

        // Exact score ties (e.g. no frequency/context data at all for either
        // side): fall back to the same structural tie-break used before, so
        // behavior stays deterministic rather than depending on discovery
        // order.
        if (left.distance != right.distance) return left.distance < right.distance;
        if (left.repeatedKey != right.repeatedKey) return left.repeatedKey;
        if (left.transposition != right.transposition) return left.transposition;
        if (left.missingCharacter != right.missingCharacter) return left.missingCharacter;
        if (left.extraCharacter != right.extraCharacter) return left.extraCharacter;
        const auto leftSize = RawSize(left);
        const auto rightSize = RawSize(right);
        const auto leftDelta = leftSize > inputSize ? leftSize - inputSize
                                                    : inputSize - leftSize;
        const auto rightDelta = rightSize > inputSize ? rightSize - inputSize
                                                      : inputSize - rightSize;
        if (leftDelta != rightDelta) return leftDelta < rightDelta;
        if (leftSize != rightSize) return leftSize < rightSize;
        return false;
    });
}

}  // namespace tekito
