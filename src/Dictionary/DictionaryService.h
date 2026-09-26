#pragma once

#include "Core/Candidate.h"

#include <optional>
#include <string>
#include <string_view>

namespace tekito::dictionary {

struct DictionaryEntry {
    std::wstring entryId;
    std::wstring headword;
    std::wstring pronunciation;
    std::wstring partOfSpeech;
    std::wstring definition;
    std::wstring example;
};

class IDictionaryProvider {
public:
    virtual ~IDictionaryProvider() = default;
    [[nodiscard]] virtual std::optional<DictionaryEntry> Find(
        std::wstring_view entryId) const = 0;
};

class IPronunciationProvider {
public:
    virtual ~IPronunciationProvider() = default;
    [[nodiscard]] virtual std::wstring Find(std::wstring_view headword) const = 0;
};

class DictionaryService final {
public:
    explicit DictionaryService(const IDictionaryProvider& provider) noexcept;
    DictionaryService(const IDictionaryProvider& provider,
                      const IPronunciationProvider& pronunciationProvider) noexcept;

    [[nodiscard]] std::optional<DictionaryEntry> Lookup(
        const Candidate& candidate) const;

private:
    const IDictionaryProvider& provider_;
    const IPronunciationProvider* pronunciationProvider_{nullptr};
};

}  // namespace tekito::dictionary
