#pragma once

#include "Core/LexiconProvider.h"
#include "Core/SymSpellIndex.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {

// Reads a generated ESDB/SCOWL word-list. One normalized word per line is
// preferred; raw ESDB scowl.txt lines are also accepted for development use.
class ExternalLexiconProvider final : public ILexiconProvider {
public:
    explicit ExternalLexiconProvider(std::filesystem::path path);

    [[nodiscard]] static std::filesystem::path DataPackRoot();
    // The folder of a Data Pack under DataPackRoot() (FindDataPack).
    [[nodiscard]] static std::filesystem::path PackDirectory(std::wstring_view packId);
    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path IndexPath(
        const std::filesystem::path& wordListPath);
    [[nodiscard]] static bool BuildIndex(
        const std::filesystem::path& wordListPath,
        const std::filesystem::path& indexPath = {},
        std::size_t stride = 256) noexcept;
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::filesystem::path& DataPath() const noexcept { return path_; }
    [[nodiscard]] const std::filesystem::path& DataIndexPath() const noexcept { return indexPath_; }
    [[nodiscard]] std::size_t IndexEntryCount() const noexcept { return index_.size(); }
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;
    void Visit(const Visitor& visitor) const override;
    void Find(const LexiconQuery& query, const Visitor& visitor) const override;

private:
    struct IndexRow {
        std::string firstWord;
        std::uint64_t offset{0};
    };

    [[nodiscard]] bool LoadIndex() noexcept;
    void BuildSymSpellIndex() noexcept;

    std::filesystem::path path_;
    std::filesystem::path indexPath_;
    std::vector<IndexRow> index_;
    mutable std::ifstream dataFile_;
    mutable std::mutex dataFileMutex_;
    bool loaded_{false};
    SymSpellIndex symSpellIndex_;
};

}  // namespace tekito
