#pragma once

#include "Core/Japanese/SortedTsv.h"
#include "Core/RankingData.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tekito {

class ExternalFrequencyProvider final : public IFrequencyProvider {
public:
    explicit ExternalFrequencyProvider(std::filesystem::path path);

    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path IndexPath(
        const std::filesystem::path& dataPath);
    [[nodiscard]] static bool BuildIndex(
        const std::filesystem::path& dataPath,
        const std::filesystem::path& indexPath = {},
        std::size_t stride = 256) noexcept;
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
    [[nodiscard]] double Score(std::wstring_view word) const noexcept override;

private:
    std::filesystem::path path_;
    // Mapped and searched in place: correction asks for hundreds of words
    // per key, and reading each from the file took about 50 us.
    japanese::SortedTsv table_;
    bool loaded_{false};
};

class ExternalPhraseContextProvider final : public IPhraseContextProvider {
public:
    explicit ExternalPhraseContextProvider(std::filesystem::path path);

    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path IndexPath(
        const std::filesystem::path& dataPath);
    [[nodiscard]] static bool BuildIndex(
        const std::filesystem::path& dataPath,
        const std::filesystem::path& indexPath = {},
        std::size_t stride = 256) noexcept;
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
    [[nodiscard]] double Score(std::wstring_view word,
                               std::wstring_view context) const noexcept override;

private:
    struct IndexRow { std::string key; std::uint64_t offset{0}; };
    struct ScoreEntry { std::string word; double score{0.0}; };
    struct CachedContextBucket {
        std::vector<ScoreEntry> entries;
        std::size_t bytes{0};
        std::list<std::string>::iterator lruPosition;
    };
    [[nodiscard]] bool LoadIndex() noexcept;
    [[nodiscard]] static std::filesystem::path RuntimeIndexPath(
        const std::filesystem::path& dataPath);
    std::filesystem::path path_;
    std::filesystem::path indexPath_;
    std::vector<IndexRow> index_;
    mutable std::ifstream dataFile_;
    mutable std::mutex dataFileMutex_;
    mutable std::list<std::string> contextLru_;
    mutable std::unordered_map<std::string, CachedContextBucket> contextCache_;
    mutable std::size_t contextCacheBytes_{0};
    bool loaded_{false};
};

}  // namespace tekito
