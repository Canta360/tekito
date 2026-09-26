#pragma once

#include "Core/LexiconProvider.h"

#include <filesystem>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace tekito {

// Reads indexed expression packs separately from spelling validity.
class ExternalSlangProvider final : public ILexiconProvider {
public:
    explicit ExternalSlangProvider(
        std::filesystem::path path,
        std::uint32_t sourceFlags = CandidateSourceTekitoOwnedSlang);

    [[nodiscard]] static std::filesystem::path DefaultPath();
    [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;
    void Visit(const Visitor& visitor) const override;
    void Find(const LexiconQuery& query, const Visitor& visitor) const override;

private:
    struct IndexRow {
        std::string raw;
        std::uint64_t offset{0};
    };

    [[nodiscard]] bool LoadIndex() noexcept;
    std::filesystem::path path_;
    std::filesystem::path indexPath_;
    std::vector<IndexRow> index_;
    mutable std::ifstream dataFile_;
    mutable std::mutex dataFileMutex_;
    std::uint32_t sourceFlags_{CandidateSourceTekitoOwnedSlang};
    bool loaded_{false};
};

}  // namespace tekito
