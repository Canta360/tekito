#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {

struct MisspellingEntry {
    std::wstring_view misspelling;
    std::wstring_view correction;
};

class IMisspellingProvider {
public:
    using Visitor = std::function<void(const MisspellingEntry&)>;

    virtual ~IMisspellingProvider() = default;
    virtual void Find(std::wstring_view misspelling, const Visitor& visitor) const = 0;
    virtual void Visit(const Visitor& visitor) const = 0;
};

class NullMisspellingProvider final : public IMisspellingProvider {
public:
    void Find(std::wstring_view, const Visitor&) const override {}
    void Visit(const Visitor&) const override {}
};

// Runtime Find() uses a local sidecar index. Visit() is retained for
// import/test tooling and does not participate in normal candidate generation.
class WikipediaCommonMisspellingsProvider final : public IMisspellingProvider {
public:
    explicit WikipediaCommonMisspellingsProvider(std::filesystem::path path);

    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] static std::filesystem::path IndexPath(
        const std::filesystem::path& packPath);
    [[nodiscard]] static bool BuildIndex(
        const std::filesystem::path& packPath,
        const std::filesystem::path& indexPath = {},
        std::size_t stride = 256) noexcept;
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }

    void Find(std::wstring_view misspelling, const Visitor& visitor) const override;
    void Visit(const Visitor& visitor) const override;

private:
    struct IndexRow {
        std::string firstKey;
        std::uint64_t offset{0};
    };

    [[nodiscard]] bool LoadIndex() noexcept;

    std::filesystem::path path_;
    std::filesystem::path indexPath_;
    std::vector<IndexRow> index_;
    mutable std::ifstream dataFile_;
    mutable std::mutex dataFileMutex_;
    bool loaded_{false};
};

}  // namespace tekito
