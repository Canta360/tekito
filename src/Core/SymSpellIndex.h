#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito {

// Symmetric-delete fuzzy lookup (Garbe's SymSpell): guarantees finding every
// indexed word within a given Damerau-Levenshtein-style edit distance,
// regardless of where in the word the edit falls, by matching precomputed
// deletions of the dictionary against deletions of the query. Unlike a
// prefix/first-character scan, this does not require the start of the word
// to be correct.
class SymSpellIndex final {
public:
    static constexpr std::size_t kMaxSupportedDistance = 2;

    // words: lowercase ASCII, ownership taken. Duplicate words are fine.
    void Build(std::vector<std::string> words);

    [[nodiscard]] bool IsBuilt() const noexcept { return !words_.empty(); }

    // Returns indexed words within maxDistance (<= kMaxSupportedDistance) of
    // query, deduplicated. Unordered; the caller ranks results.
    [[nodiscard]] std::vector<std::string_view> Lookup(std::string_view query,
                                                        std::size_t maxDistance) const;

private:
    std::vector<std::string> words_;
    std::unordered_map<std::string, std::vector<std::uint32_t>> deletes_;
};

}  // namespace tekito
