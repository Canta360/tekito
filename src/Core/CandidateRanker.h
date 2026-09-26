#pragma once

#include "Core/BuiltinLexicon.h"
#include "Core/RankingData.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tekito {

struct CorrectionCandidate {
    std::size_t distance;
    bool repeatedKey;
    bool transposition;
    bool missingCharacter;
    bool extraCharacter;
    bool qwertyNeighbor{false};
    LexiconEntry entry;
    std::wstring rawText;
    RankingSignals signals{};
    bool sourceBackedCorrection{false};
    bool weakExactRecovery{false};
    bool validWordConflict{false};
    double weightedCost{0.0};
};

class CandidateRanker final {
public:
    void RankCorrections(std::vector<CorrectionCandidate>& candidates,
                         std::size_t inputSize) const;
};

}  // namespace tekito
