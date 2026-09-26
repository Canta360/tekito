#include "Core/UserLexiconProvider.h"

namespace tekito {

UserLexiconProvider::UserLexiconProvider(const UserDictionary& dictionary)
    : dictionary_(dictionary) {
    Refresh();
}

void UserLexiconProvider::Refresh() {
    const auto userEntries = dictionary_.Entries();
    rawTexts_.clear();
    candidateTexts_.clear();
    entries_.clear();
    rawTexts_.reserve(userEntries.size());
    candidateTexts_.reserve(userEntries.size());
    entries_.reserve(userEntries.size());

    for (const auto& userEntry : userEntries) {
        if (!userEntry.enabled) continue;
        rawTexts_.push_back(userEntry.raw);
        candidateTexts_.push_back(userEntry.candidate);

        entries_.push_back({rawTexts_.back(), candidateTexts_.back(),
                            SemanticLabel::Original, SemanticLabel::Standard,
                            false, false, CandidatePolicySuggestOnly,
                            userEntry.policyFlags, CandidateSourceUserDictionary,
                            userEntry.caseSensitive});
    }
}

std::span<const LexiconEntry> UserLexiconProvider::Entries() const noexcept {
    return entries_;
}

}  // namespace tekito
