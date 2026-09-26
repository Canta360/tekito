#include "Dictionary/DictionaryService.h"

namespace tekito::dictionary {

DictionaryService::DictionaryService(const IDictionaryProvider& provider) noexcept
    : provider_(provider) {}

DictionaryService::DictionaryService(
    const IDictionaryProvider& provider,
    const IPronunciationProvider& pronunciationProvider) noexcept
    : provider_(provider), pronunciationProvider_(&pronunciationProvider) {}

std::optional<DictionaryEntry> DictionaryService::Lookup(
    const Candidate& candidate) const {
    if (candidate.dictionaryEntryId.empty()) return std::nullopt;
    auto entry = provider_.Find(candidate.dictionaryEntryId);
    if (entry && entry->pronunciation.empty() && pronunciationProvider_) {
        entry->pronunciation = pronunciationProvider_->Find(entry->headword);
    }
    return entry;
}

}  // namespace tekito::dictionary
