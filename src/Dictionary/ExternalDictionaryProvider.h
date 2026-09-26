#pragma once

#include "Dictionary/DictionaryService.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tekito::dictionary {

// Local, indexed dictionary-display data. Definitions are read only when a
// candidate is inspected; the pack is not loaded into memory at startup.
class ExternalDictionaryProvider final : public IDictionaryProvider {
public:
    explicit ExternalDictionaryProvider(std::filesystem::path path);

    [[nodiscard]] static std::filesystem::path DataPackRoot();
    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path IndexPath(
        const std::filesystem::path& dataPath);
    [[nodiscard]] static bool BuildIndex(
        const std::filesystem::path& dataPath,
        const std::filesystem::path& indexPath = {},
        std::size_t stride = 256) noexcept;
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }

    [[nodiscard]] std::optional<DictionaryEntry> Find(
        std::wstring_view entryId) const override;

private:
    struct IndexRow {
        std::string key;
        std::uint64_t offset{0};
    };

    [[nodiscard]] bool LoadIndex() noexcept;

    std::filesystem::path path_;
    std::filesystem::path indexPath_;
    std::vector<IndexRow> index_;
    bool loaded_{false};
};

class ExternalPronunciationProvider final : public IPronunciationProvider {
public:
    explicit ExternalPronunciationProvider(std::filesystem::path path);

    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
    [[nodiscard]] std::wstring Find(std::wstring_view headword) const override;

private:
    struct IndexRow {
        std::string key;
        std::uint64_t offset{0};
    };

    [[nodiscard]] bool LoadIndex() noexcept;
    std::filesystem::path path_;
    std::filesystem::path indexPath_;
    std::vector<IndexRow> index_;
    bool loaded_{false};
};

}  // namespace tekito::dictionary
