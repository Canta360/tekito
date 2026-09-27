#pragma once

#include "UserData/UserDataRepository.h"

#include <filesystem>

struct sqlite3;

namespace tekito::userdata {

class SqliteUserDictionaryRepository final : public IUserDataRepository {
public:
    explicit SqliteUserDictionaryRepository(std::filesystem::path databasePath);
    ~SqliteUserDictionaryRepository();

    SqliteUserDictionaryRepository(const SqliteUserDictionaryRepository&) = delete;
    SqliteUserDictionaryRepository& operator=(const SqliteUserDictionaryRepository&) = delete;

    [[nodiscard]] bool Open() noexcept override;
    void Close() noexcept override;
    [[nodiscard]] bool Load(UserDictionary& dictionary) const noexcept override;
    [[nodiscard]] bool Save(const UserDictionary& dictionary) noexcept override;
    [[nodiscard]] bool LoadSettings(UserSettings& settings) const noexcept override;
    [[nodiscard]] bool SaveSettings(const UserSettings& settings) noexcept override;
    [[nodiscard]] bool LoadLearning(UserLearningStore& learning) const noexcept override;
    [[nodiscard]] bool SaveLearning(const UserLearningStore& learning) noexcept override;
    [[nodiscard]] bool ResetLearning() noexcept override;
    [[nodiscard]] bool LoadSocialLearning(SocialLearningStore& learning) const noexcept override;
    [[nodiscard]] bool SaveSocialLearning(const SocialLearningStore& learning) noexcept override;
    [[nodiscard]] bool ResetSocialLearning() noexcept override;
    [[nodiscard]] bool LoadJapaneseLearning(japanese::JapaneseLearningStore& learning) const noexcept override;
    [[nodiscard]] bool SaveJapaneseLearning(const japanese::JapaneseLearningStore& learning) noexcept override;
    [[nodiscard]] bool ResetJapaneseLearning() noexcept override;
    [[nodiscard]] bool IsOpen() const noexcept override;

private:
    [[nodiscard]] bool EnsureSchema() noexcept;

    std::filesystem::path databasePath_;
    sqlite3* database_{nullptr};
};

[[nodiscard]] std::filesystem::path DefaultUserDatabasePath();

}  // namespace tekito::userdata
