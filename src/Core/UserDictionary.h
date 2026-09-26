#pragma once

#include "Core/Candidate.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tekito {

using UserDictionaryEntryId = std::uint64_t;

struct UserDictionaryEntry {
    UserDictionaryEntryId id{0};
    std::wstring raw;
    std::wstring candidate;
    std::uint32_t policyFlags{CandidatePolicyNone};
    bool caseSensitive{false};
    bool enabled{true};
};

class UserDictionary final {
public:
    [[nodiscard]] bool Add(UserDictionaryEntry entry);
    [[nodiscard]] bool Remove(UserDictionaryEntryId id) noexcept;
    [[nodiscard]] const UserDictionaryEntry* Find(UserDictionaryEntryId id) const noexcept;
    [[nodiscard]] std::span<const UserDictionaryEntry> Entries() const noexcept;

private:
    std::vector<UserDictionaryEntry> entries_;
};

}  // namespace tekito
