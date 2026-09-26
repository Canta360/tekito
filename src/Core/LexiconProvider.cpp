#include "Core/LexiconProvider.h"

#include <algorithm>
#include <cwctype>
#include <string>

namespace tekito {

namespace {

std::wstring Lower(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

bool Matches(const LexiconQuery& query, const LexiconEntry& entry) {
    const auto queryText = query.text;
    const auto entryText = entry.raw;
    if (entry.caseSensitive) {
        if (query.kind == LexiconQuery::Kind::Exact) return queryText == entryText;
        if (query.kind == LexiconQuery::Kind::Candidate) {
            return !queryText.empty() && !entryText.empty() && queryText.front() == entryText.front();
        }
        return entryText.starts_with(queryText);
    }

    const auto normalizedQuery = Lower(queryText);
    const auto normalizedEntry = Lower(entryText);
    if (query.kind == LexiconQuery::Kind::Exact) return normalizedQuery == normalizedEntry;
    if (query.kind == LexiconQuery::Kind::Candidate) {
        return !normalizedQuery.empty() && !normalizedEntry.empty() &&
               normalizedQuery.front() == normalizedEntry.front();
    }
    return normalizedEntry.starts_with(normalizedQuery);
}

}  // namespace

void ILexiconProvider::Find(const LexiconQuery& query, const Visitor& visitor) const {
    std::size_t resultCount = 0;
    Visit([&](const auto& entry) {
        if (query.maxResults != 0 && resultCount >= query.maxResults) return;
        if (!Matches(query, entry)) return;
        visitor(entry);
        ++resultCount;
    });
}

std::span<const LexiconEntry> BuiltinLexiconProvider::Entries() const noexcept {
    return BuiltinLexicon();
}

CompositeLexiconProvider::CompositeLexiconProvider(const ILexiconProvider& primary,
                                                   const ILexiconProvider& fallback)
    : primary_(primary), fallback_(fallback) {
    Refresh();
}

void CompositeLexiconProvider::Refresh() {
    const auto primaryEntries = primary_.Entries();
    const auto fallbackEntries = fallback_.Entries();
    entries_.clear();
    entries_.reserve(primaryEntries.size() + fallbackEntries.size());
    entries_.insert(entries_.end(), primaryEntries.begin(), primaryEntries.end());
    entries_.insert(entries_.end(), fallbackEntries.begin(), fallbackEntries.end());
}

std::span<const LexiconEntry> CompositeLexiconProvider::Entries() const noexcept {
    return entries_;
}

void CompositeLexiconProvider::Visit(const Visitor& visitor) const {
    primary_.Visit(visitor);
    fallback_.Visit(visitor);
}

void CompositeLexiconProvider::Find(const LexiconQuery& query, const Visitor& visitor) const {
    primary_.Find(query, visitor);
    fallback_.Find(query, visitor);
}

}  // namespace tekito
