#pragma once

#include "Core/Japanese/JapaneseConverter.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

struct JapaneseLearningEntry {
    std::wstring reading;
    std::wstring surface;
    // Times the user settled on this surface for the reading, and times it
    // was offered first but the user chose something else.
    double selections{0};
    double rejections{0};
    std::uint64_t lastUsed{0};
};

// Which way the user writes each reading, learned from the phrases they
// commit. A surface chosen once moves to the top; one chosen many times is
// not displaced by a single different choice, since each time it is passed
// over counts half as much as a choice.
class JapaneseLearningStore final {
public:
    static constexpr std::size_t kMaxEntries = 20000;

    // The user committed `chosen` for `reading` when `shownFirst` was offered
    // first.
    void RecordChoice(std::wstring_view reading, std::wstring_view chosen, std::wstring_view shownFirst);
    // Candidates the user prefers for this reading go first, most preferred
    // first; the rest keep their order.
    void Reorder(std::wstring_view reading, std::vector<PhraseCandidate>& candidates) const;
    [[nodiscard]] double Preference(std::wstring_view reading, std::wstring_view surface) const;

    // For loading; false if the entry is malformed.
    bool Add(JapaneseLearningEntry entry);
    [[nodiscard]] std::vector<JapaneseLearningEntry> Entries() const;
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }
    void Clear() noexcept;

private:
    [[nodiscard]] static std::wstring Key(std::wstring_view reading, std::wstring_view surface);
    [[nodiscard]] const JapaneseLearningEntry* Find(std::wstring_view reading, std::wstring_view surface) const;
    void EvictOldest();

    std::map<std::wstring, JapaneseLearningEntry, std::less<>> entries_;
    std::uint64_t clock_{0};
};

}  // namespace tekito::japanese
