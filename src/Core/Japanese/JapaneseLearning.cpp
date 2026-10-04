#include "Core/Japanese/JapaneseLearning.h"

#include <algorithm>
#include <cmath>

namespace tekito::japanese {

std::wstring JapaneseLearningStore::Key(std::wstring_view reading, std::wstring_view surface) {
    std::wstring key(reading);
    key += L'\x1f';
    key += surface;
    return key;
}

const JapaneseLearningEntry* JapaneseLearningStore::Find(std::wstring_view reading,
                                                         std::wstring_view surface) const {
    const auto it = entries_.find(Key(reading, surface));
    return it == entries_.end() ? nullptr : &it->second;
}

bool JapaneseLearningStore::Forget(std::wstring_view reading, std::wstring_view surface) {
    const auto it = entries_.find(Key(reading, surface));
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

void JapaneseLearningStore::RecordChoice(std::wstring_view reading, std::wstring_view chosen,
                                         std::wstring_view shownFirst) {
    if (reading.empty() || chosen.empty()) return;
    auto& entry = entries_[Key(reading, chosen)];
    if (entry.reading.empty()) {
        entry.reading = reading;
        entry.surface = chosen;
    }
    entry.selections += 1;
    entry.lastUsed = ++clock_;
    // Passing over what was offered first counts against it, if it had
    // been chosen before.
    if (!shownFirst.empty() && shownFirst != chosen) {
        const auto first = entries_.find(Key(reading, shownFirst));
        if (first != entries_.end()) first->second.rejections += 1;
    }
    if (entries_.size() > kMaxEntries) EvictOldest();
}

double JapaneseLearningStore::Preference(std::wstring_view reading, std::wstring_view surface) const {
    const auto* entry = Find(reading, surface);
    return entry ? entry->selections - 0.5 * entry->rejections : 0.0;
}

void JapaneseLearningStore::Reorder(std::wstring_view reading, std::vector<PhraseCandidate>& candidates) const {
    if (entries_.empty() || candidates.size() < 2) return;
    struct Ranked {
        double preference;
        std::uint64_t lastUsed;
        std::size_t index;
    };
    std::vector<Ranked> preferred;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto* entry = Find(reading, candidates[i].text);
        if (!entry) continue;
        const double preference = entry->selections - 0.5 * entry->rejections;
        if (preference > 0) preferred.push_back({preference, entry->lastUsed, i});
    }
    if (preferred.empty()) return;
    std::stable_sort(preferred.begin(), preferred.end(), [](const Ranked& a, const Ranked& b) {
        if (a.preference != b.preference) return a.preference > b.preference;
        return a.lastUsed > b.lastUsed;
    });
    std::vector<PhraseCandidate> reordered;
    reordered.reserve(candidates.size());
    std::vector<bool> moved(candidates.size(), false);
    for (const auto& ranked : preferred) {
        reordered.push_back(std::move(candidates[ranked.index]));
        moved[ranked.index] = true;
    }
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!moved[i]) reordered.push_back(std::move(candidates[i]));
    }
    candidates = std::move(reordered);
}

std::vector<JapaneseLearningEntry> JapaneseLearningStore::StartingWith(std::wstring_view prefix,
                                                                       std::size_t limit) const {
    std::vector<JapaneseLearningEntry> found;
    if (prefix.empty()) return found;
    for (auto it = entries_.lower_bound(prefix); it != entries_.end() && it->first.starts_with(prefix); ++it) {
        const auto& entry = it->second;
        if (entry.reading.size() > prefix.size() && entry.selections - 0.5 * entry.rejections > 0) {
            found.push_back(entry);
        }
    }
    std::sort(found.begin(), found.end(), [](const JapaneseLearningEntry& a, const JapaneseLearningEntry& b) {
        const double pa = a.selections - 0.5 * a.rejections;
        const double pb = b.selections - 0.5 * b.rejections;
        if (pa != pb) return pa > pb;
        return a.lastUsed > b.lastUsed;
    });
    if (found.size() > limit) found.resize(limit);
    return found;
}

bool JapaneseLearningStore::Add(JapaneseLearningEntry entry) {
    if (entry.reading.empty() || entry.surface.empty() || !std::isfinite(entry.selections) ||
        !std::isfinite(entry.rejections) || entry.selections < 0 || entry.rejections < 0) {
        return false;
    }
    clock_ = std::max(clock_, entry.lastUsed);
    auto key = Key(entry.reading, entry.surface);
    entries_[std::move(key)] = std::move(entry);
    if (entries_.size() > kMaxEntries) EvictOldest();
    return true;
}

std::vector<JapaneseLearningEntry> JapaneseLearningStore::Entries() const {
    std::vector<JapaneseLearningEntry> entries;
    entries.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) entries.push_back(entry);
    return entries;
}

void JapaneseLearningStore::Clear() noexcept {
    entries_.clear();
    clock_ = 0;
}

void JapaneseLearningStore::EvictOldest() {
    auto oldest = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
    }
    if (oldest != entries_.end()) entries_.erase(oldest);
}

}  // namespace tekito::japanese
