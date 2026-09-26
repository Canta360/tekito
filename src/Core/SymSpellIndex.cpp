#include "Core/SymSpellIndex.h"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_set>

namespace tekito {
namespace {

// Emits every distinct string reachable by deleting 1..maxDepth characters
// from `word` (any positions, any order), via depth-first deletion. `seen`
// dedupes within one call so e.g. deleting position 0 then 2 and deleting
// position 2 then 0 (which can coincide) is only emitted once.
void CollectDeletes(std::string_view word, std::size_t remainingDepth,
                    std::unordered_set<std::string>& seen,
                    const std::function<void(const std::string&)>& emit) {
    if (remainingDepth == 0) return;
    for (std::size_t index = 0; index < word.size(); ++index) {
        std::string variant;
        variant.reserve(word.size() - 1);
        variant.append(word.substr(0, index));
        variant.append(word.substr(index + 1));
        if (seen.insert(variant).second) {
            emit(variant);
            CollectDeletes(variant, remainingDepth - 1, seen, emit);
        }
    }
}

// Bounded Damerau-ish (insert/delete/substitute; no transposition) edit
// distance, same recurrence as ExternalLexiconProvider's Near(): returns a
// value > limit as soon as every cell in a row exceeds it.
std::size_t BoundedEditDistance(std::string_view left, std::string_view right,
                                std::size_t limit) {
    if (left.size() > right.size() + limit || right.size() > left.size() + limit) {
        return limit + 1;
    }
    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t column = 0; column <= right.size(); ++column) previous[column] = column;
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        std::size_t best = current[0];
        for (std::size_t column = 1; column <= right.size(); ++column) {
            current[column] = std::min({current[column - 1] + 1, previous[column] + 1,
                                        previous[column - 1] +
                                            (left[row - 1] == right[column - 1] ? 0 : 1)});
            best = std::min(best, current[column]);
        }
        if (best > limit) return limit + 1;
        std::swap(previous, current);
    }
    return previous[right.size()];
}

}  // namespace

void SymSpellIndex::Build(std::vector<std::string> words) {
    words_ = std::move(words);
    deletes_.clear();
    deletes_.reserve(words_.size() * 4);

    std::unordered_set<std::string> seenForWord;
    for (std::uint32_t index = 0; index < words_.size(); ++index) {
        const auto& word = words_[index];
        auto& exactBucket = deletes_[word];
        if (exactBucket.empty() || exactBucket.back() != index) exactBucket.push_back(index);

        seenForWord.clear();
        seenForWord.insert(word);
        CollectDeletes(word, kMaxSupportedDistance, seenForWord, [&](const std::string& variant) {
            auto& bucket = deletes_[variant];
            if (bucket.empty() || bucket.back() != index) bucket.push_back(index);
        });
    }
}

std::vector<std::string_view> SymSpellIndex::Lookup(std::string_view query,
                                                     std::size_t maxDistance) const {
    std::vector<std::string_view> results;
    if (words_.empty() || query.empty()) return results;
    maxDistance = std::min(maxDistance, kMaxSupportedDistance);

    std::unordered_set<std::uint32_t> candidateIndices;
    const auto considerBucket = [&](const std::string& key) {
        const auto it = deletes_.find(key);
        if (it == deletes_.end()) return;
        for (const auto wordIndex : it->second) candidateIndices.insert(wordIndex);
    };

    const std::string queryOwned(query);
    considerBucket(queryOwned);
    std::unordered_set<std::string> seenForQuery;
    seenForQuery.insert(queryOwned);
    CollectDeletes(queryOwned, maxDistance, seenForQuery,
                   [&](const std::string& variant) { considerBucket(variant); });

    results.reserve(candidateIndices.size());
    for (const auto wordIndex : candidateIndices) {
        const auto& word = words_[wordIndex];
        if (BoundedEditDistance(query, word, maxDistance) <= maxDistance) {
            results.emplace_back(word);
        }
    }
    // candidateIndices is a hash set, so iteration order (and therefore any
    // downstream stable-sort tie-break that happens to reach discovery order)
    // would otherwise be nondeterministic between runs.
    std::sort(results.begin(), results.end());
    return results;
}

}  // namespace tekito
