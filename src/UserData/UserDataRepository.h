#pragma once

#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/UserDictionary.h"
#include "Core/UserLearning.h"
#include "UserData/UserSettings.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace tekito::userdata {

// A conversion TEKITO wrote, kept for a while so it can be reported as wrong.
struct RecentConversion {
    std::int64_t id{0};
    std::wstring reading;  // the reading typed (きかいがうごく)
    std::wstring text;     // what it became
    std::wstring mode;   // "Japanese" or "Mix"
    std::int64_t time{0};  // seconds since 1970
};

class IUserDataRepository {
public:
    virtual ~IUserDataRepository() = default;

    [[nodiscard]] virtual bool Open() noexcept = 0;
    virtual void Close() noexcept = 0;
    [[nodiscard]] virtual bool Load(UserDictionary& dictionary) const noexcept = 0;
    [[nodiscard]] virtual bool Save(const UserDictionary& dictionary) noexcept = 0;
    [[nodiscard]] virtual bool LoadSettings(UserSettings& settings) const noexcept = 0;
    [[nodiscard]] virtual bool SaveSettings(const UserSettings& settings) noexcept = 0;
    [[nodiscard]] virtual bool LoadLearning(UserLearningStore& learning) const noexcept = 0;
    [[nodiscard]] virtual bool SaveLearning(const UserLearningStore& learning) noexcept = 0;
    [[nodiscard]] virtual bool ResetLearning() noexcept = 0;
    [[nodiscard]] virtual bool LoadSocialLearning(SocialLearningStore& learning) const noexcept = 0;
    [[nodiscard]] virtual bool SaveSocialLearning(const SocialLearningStore& learning) noexcept = 0;
    [[nodiscard]] virtual bool ResetSocialLearning() noexcept = 0;
    [[nodiscard]] virtual bool LoadJapaneseLearning(japanese::JapaneseLearningStore& learning) const noexcept = 0;
    [[nodiscard]] virtual bool SaveJapaneseLearning(const japanese::JapaneseLearningStore& learning) noexcept = 0;
    [[nodiscard]] virtual bool ResetJapaneseLearning() noexcept = 0;
    // The words the user added for Japanese (reading, surface, kind),
    // separate from the English user dictionary.
    [[nodiscard]] virtual bool LoadJapaneseUserWords(std::vector<japanese::UserWord>& words) const noexcept = 0;
    [[nodiscard]] virtual bool SaveJapaneseUserWords(const std::vector<japanese::UserWord>& words) noexcept = 0;
    // The latest conversions, newest first; adding keeps only `keep` of them.
    [[nodiscard]] virtual bool AddRecentConversion(const RecentConversion& conversion, std::size_t keep) noexcept = 0;
    [[nodiscard]] virtual bool LoadRecentConversions(std::vector<RecentConversion>& conversions) const noexcept = 0;
    // id 0: all of them.
    [[nodiscard]] virtual bool ForgetRecentConversion(std::int64_t id) noexcept = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IUserDataRepository> CreateDefaultUserDataRepository() noexcept;
[[nodiscard]] std::filesystem::path DefaultUserDatabasePath();

}  // namespace tekito::userdata
