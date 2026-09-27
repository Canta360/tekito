#include "UserData/SqliteUserDictionaryRepository.h"

#include "sqlite3.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <utility>

namespace tekito::userdata {
namespace {

constexpr const char* kCreateSchema =
    "CREATE TABLE IF NOT EXISTS user_dictionary ("
    "id INTEGER PRIMARY KEY NOT NULL,"
    "raw TEXT NOT NULL,"
    "candidate TEXT NOT NULL,"
    "policy_flags INTEGER NOT NULL DEFAULT 0,"
    "case_sensitive INTEGER NOT NULL DEFAULT 0,"
    "enabled INTEGER NOT NULL DEFAULT 1"
    ");"
    "CREATE TABLE IF NOT EXISTS settings ("
    "key TEXT PRIMARY KEY NOT NULL,"
    "value INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS user_learning ("
    "word TEXT PRIMARY KEY NOT NULL,"
    "candidate_selections INTEGER NOT NULL DEFAULT 0,"
    "raw_keeps INTEGER NOT NULL DEFAULT 0,"
    "undo_count INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS user_learning_preferences ("
    "raw_text TEXT NOT NULL,"
    "candidate TEXT NOT NULL,"
    "alpha REAL NOT NULL DEFAULT 0,"
    "beta REAL NOT NULL DEFAULT 0,"
    "exposure INTEGER NOT NULL DEFAULT 0,"
    "direct_event_mass REAL NOT NULL DEFAULT 0,"
    "PRIMARY KEY(raw_text, candidate)"
    ");"
    "CREATE TABLE IF NOT EXISTS social_learning ("
    "trigger TEXT NOT NULL," 
    "candidate TEXT NOT NULL," 
    "alpha REAL NOT NULL DEFAULT 0," 
    "beta REAL NOT NULL DEFAULT 0," 
    "exposure INTEGER NOT NULL DEFAULT 0," 
    "direct_event_mass REAL NOT NULL DEFAULT 0," 
    "PRIMARY KEY(trigger, candidate)"
    ");"
    "CREATE TABLE IF NOT EXISTS japanese_learning ("
    "reading TEXT NOT NULL,"
    "surface TEXT NOT NULL,"
    "selections REAL NOT NULL DEFAULT 0,"
    "rejections REAL NOT NULL DEFAULT 0,"
    "last_used INTEGER NOT NULL DEFAULT 0,"
    "PRIMARY KEY (reading, surface)"
    ");"
    "CREATE TABLE IF NOT EXISTS excluded_apps ("
    "name TEXT PRIMARY KEY NOT NULL COLLATE NOCASE"
    ");";

bool BindText16(sqlite3_stmt* statement, int index, const std::wstring& value) noexcept {
    return sqlite3_bind_text16(statement, index, value.c_str(),
                               static_cast<int>(value.size() * sizeof(wchar_t)),
                               SQLITE_TRANSIENT) == SQLITE_OK;
}

}  // namespace

SqliteUserDictionaryRepository::SqliteUserDictionaryRepository(
    std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)) {}

SqliteUserDictionaryRepository::~SqliteUserDictionaryRepository() {
    Close();
}

bool SqliteUserDictionaryRepository::Open() noexcept {
    if (database_) return true;

    try {
        if (databasePath_.has_parent_path()) {
            std::filesystem::create_directories(databasePath_.parent_path());
        }
    } catch (...) {
        return false;
    }

    sqlite3* database = nullptr;
    if (sqlite3_open16(databasePath_.c_str(), &database) != SQLITE_OK) {
        if (database) sqlite3_close(database);
        return false;
    }
    database_ = database;
    if (!EnsureSchema()) {
        Close();
        return false;
    }
    return true;
}

void SqliteUserDictionaryRepository::Close() noexcept {
    if (!database_) return;
    sqlite3_close(database_);
    database_ = nullptr;
}

bool SqliteUserDictionaryRepository::Load(UserDictionary& dictionary) const noexcept {
    if (!database_) return false;

    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "SELECT id, raw, candidate, policy_flags, case_sensitive, enabled "
        "FROM user_dictionary ORDER BY id;";
    if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    UserDictionary loaded;
    bool success = true;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* raw = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 1));
        const auto* candidate = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 2));
        if (!raw || !candidate) {
            success = false;
            break;
        }

        UserDictionaryEntry entry;
        entry.id = static_cast<UserDictionaryEntryId>(sqlite3_column_int64(statement, 0));
        entry.raw = raw;
        entry.candidate = candidate;
        entry.policyFlags = static_cast<std::uint32_t>(sqlite3_column_int(statement, 3));
        entry.caseSensitive = sqlite3_column_int(statement, 4) != 0;
        entry.enabled = sqlite3_column_int(statement, 5) != 0;
        if (!loaded.Add(std::move(entry))) {
            success = false;
            break;
        }
    }
    success = success && stepResult == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (success) dictionary = std::move(loaded);
    return success;
}

bool SqliteUserDictionaryRepository::Save(const UserDictionary& dictionary) noexcept {
    if (!database_) return false;
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    bool success = sqlite3_exec(database_, "DELETE FROM user_dictionary;", nullptr, nullptr,
                                nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "INSERT INTO user_dictionary "
        "(id, raw, candidate, policy_flags, case_sensitive, enabled) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    if (success && sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) {
        success = false;
    }

    if (success) {
        for (const auto& entry : dictionary.Entries()) {
            success = sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(entry.id)) == SQLITE_OK &&
                      BindText16(statement, 2, entry.raw) &&
                      BindText16(statement, 3, entry.candidate) &&
                      sqlite3_bind_int(statement, 4, static_cast<int>(entry.policyFlags)) == SQLITE_OK &&
                      sqlite3_bind_int(statement, 5, entry.caseSensitive ? 1 : 0) == SQLITE_OK &&
                      sqlite3_bind_int(statement, 6, entry.enabled ? 1 : 0) == SQLITE_OK &&
                      sqlite3_step(statement) == SQLITE_DONE;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if (!success) break;
        }
    }
    if (statement) sqlite3_finalize(statement);

    const char* transaction = success ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(database_, transaction, nullptr, nullptr, nullptr) != SQLITE_OK) {
        success = false;
    }
    return success;
}

bool SqliteUserDictionaryRepository::LoadSettings(UserSettings& settings) const noexcept {
    if (!database_) return false;

    sqlite3_stmt* statement = nullptr;
    constexpr const char* query = "SELECT key, value FROM settings;";
    if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }

    UserSettings loaded;
    bool success = true;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        if (!key) {
            success = false;
            break;
        }
        const int value = sqlite3_column_int(statement, 1);
        if (std::strcmp(key, "default_input_mode") == 0) {
            loaded.defaultInputMode = value == static_cast<int>(InputMode::Direct)
                                          ? InputMode::Direct
                                          : InputMode::Convert;
        } else if (std::strcmp(key, "last_input_mode") == 0) {
            loaded.lastInputMode = value == static_cast<int>(InputMode::Direct)
                                       ? InputMode::Direct
                                       : InputMode::Convert;
        } else if (std::strcmp(key, "restore_last_input_mode") == 0) {
            loaded.restoreLastInputMode = value != 0;
        } else if (std::strcmp(key, "learning_enabled") == 0) {
            loaded.learningEnabled = value != 0;
        } else if (std::strcmp(key, "correction_enabled") == 0) {
            loaded.correctionEnabled = value != 0;
        } else if (std::strcmp(key, "common_misspellings_enabled") == 0) {
            loaded.commonMisspellingsEnabled = value != 0;
        } else if (std::strcmp(key, "context_suggestions_enabled") == 0) {
            loaded.contextSuggestionsEnabled = value != 0;
        } else if (std::strcmp(key, "completion_enabled") == 0) {
            loaded.completionEnabled = value != 0;
        } else if (std::strcmp(key, "candidate_window_enabled") == 0) {
            loaded.candidateWindowEnabled = value != 0;
        } else if (std::strcmp(key, "japanese_phonetic_suggestions_enabled") == 0) {
            loaded.japanesePhoneticSuggestionsEnabled = value != 0;
        } else if (std::strcmp(key, "social_expression_range") == 0) {
            loaded.socialExpressionRange = std::clamp(value, 0, 4);
        } else if (std::strcmp(key, "social_personalization") == 0) {
            loaded.socialPersonalization = std::clamp(value, 0, 2);
        } else if (std::strcmp(key, "candidate_window_style") == 0) {
            loaded.candidateWindowStyle = std::clamp(value, 0, 1);
        } else if (std::strcmp(key, "toggle_key") == 0) {
            loaded.toggleKey = std::clamp(value, 0, 3);
        } else if (std::strcmp(key, "period_on_enter") == 0) {
            loaded.periodOnEnter = value != 0;
        } else if (std::strcmp(key, "ui_language") == 0) {
            loaded.uiLanguage = std::clamp(value, 0, 2);
        } else if (std::strcmp(key, "keyboard_type") == 0) {
            loaded.keyboardType = std::clamp(value, 0, 2);
        } else if (std::strcmp(key, "last_japanese_profile_mode") == 0) {
            loaded.lastJapaneseProfileMode =
                value == static_cast<int>(InputMode::Direct)    ? InputMode::Direct
                : value == static_cast<int>(InputMode::Convert) ? InputMode::Convert
                                                                : InputMode::Japanese;
        } else if (std::strcmp(key, "japanese_profile_english_mode") == 0) {
            loaded.japaneseProfileEnglishMode = value == static_cast<int>(InputMode::Direct)
                                                    ? InputMode::Direct
                                                    : InputMode::Convert;
        } else if (std::strcmp(key, "japanese_space_width") == 0) {
            loaded.japaneseSpaceWidth = std::clamp(value, 0, 2);
        } else if (std::strcmp(key, "japanese_punctuation") == 0) {
            loaded.japanesePunctuation = std::clamp(value, 0, 3);
        } else if (std::strcmp(key, "japanese_prediction_enabled") == 0) {
            loaded.japanesePredictionEnabled = value != 0;
        }
    }
    success = success && stepResult == SQLITE_DONE;
    sqlite3_finalize(statement);

    if (success) {
        statement = nullptr;
        constexpr const char* appsQuery = "SELECT name FROM excluded_apps ORDER BY name;";
        if (sqlite3_prepare_v2(database_, appsQuery, -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        try {
            while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
                const auto* name = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
                if (name && *name) loaded.excludedApps.emplace_back(name);
            }
        } catch (...) {
            stepResult = SQLITE_NOMEM;
        }
        success = stepResult == SQLITE_DONE;
        sqlite3_finalize(statement);
    }
    if (success) settings = std::move(loaded);
    return success;
}

bool SqliteUserDictionaryRepository::SaveSettings(const UserSettings& settings) noexcept {
    if (!database_) return false;
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    bool success = true;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) {
        success = false;
    }

    const auto saveValue = [&](const char* key, int value) {
        if (!success) return;
        success = sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
                  sqlite3_bind_int(statement, 2, value) == SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    saveValue("default_input_mode", static_cast<int>(settings.defaultInputMode));
    saveValue("last_input_mode", static_cast<int>(settings.lastInputMode));
    saveValue("restore_last_input_mode", settings.restoreLastInputMode ? 1 : 0);
    saveValue("learning_enabled", settings.learningEnabled ? 1 : 0);
    saveValue("correction_enabled", settings.correctionEnabled ? 1 : 0);
    saveValue("common_misspellings_enabled", settings.commonMisspellingsEnabled ? 1 : 0);
    saveValue("context_suggestions_enabled", settings.contextSuggestionsEnabled ? 1 : 0);
    saveValue("completion_enabled", settings.completionEnabled ? 1 : 0);
    saveValue("candidate_window_enabled", settings.candidateWindowEnabled ? 1 : 0);
    saveValue("japanese_phonetic_suggestions_enabled",
              settings.japanesePhoneticSuggestionsEnabled ? 1 : 0);
    saveValue("social_expression_range", std::clamp(settings.socialExpressionRange, 0, 4));
    saveValue("social_personalization", std::clamp(settings.socialPersonalization, 0, 2));
    saveValue("candidate_window_style", std::clamp(settings.candidateWindowStyle, 0, 1));
    saveValue("toggle_key", std::clamp(settings.toggleKey, 0, 3));
    saveValue("period_on_enter", settings.periodOnEnter ? 1 : 0);
    saveValue("ui_language", std::clamp(settings.uiLanguage, 0, 2));
    saveValue("keyboard_type", std::clamp(settings.keyboardType, 0, 2));
    saveValue("last_japanese_profile_mode", static_cast<int>(settings.lastJapaneseProfileMode));
    saveValue("japanese_profile_english_mode",
              settings.japaneseProfileEnglishMode == InputMode::Direct
                  ? static_cast<int>(InputMode::Direct)
                  : static_cast<int>(InputMode::Convert));
    saveValue("japanese_space_width", std::clamp(settings.japaneseSpaceWidth, 0, 2));
    saveValue("japanese_punctuation", std::clamp(settings.japanesePunctuation, 0, 3));
    saveValue("japanese_prediction_enabled", settings.japanesePredictionEnabled ? 1 : 0);
    if (statement) sqlite3_finalize(statement);
    statement = nullptr;

    if (success) {
        success = sqlite3_exec(database_, "DELETE FROM excluded_apps;", nullptr, nullptr,
                               nullptr) == SQLITE_OK;
    }
    if (success) {
        constexpr const char* appsQuery =
            "INSERT OR IGNORE INTO excluded_apps (name) VALUES (?);";
        success = sqlite3_prepare_v2(database_, appsQuery, -1, &statement, nullptr) == SQLITE_OK;
        for (const auto& name : settings.excludedApps) {
            if (!success) break;
            if (name.empty()) continue;
            success = BindText16(statement, 1, name) && sqlite3_step(statement) == SQLITE_DONE;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
        if (statement) sqlite3_finalize(statement);
    }

    const char* transaction = success ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(database_, transaction, nullptr, nullptr, nullptr) != SQLITE_OK) {
        success = false;
    }
    return success;
}

bool SqliteUserDictionaryRepository::LoadLearning(UserLearningStore& learning) const noexcept {
    if (!database_) return false;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "SELECT word, candidate_selections, raw_keeps, undo_count FROM user_learning ORDER BY word;";
    if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) return false;

    UserLearningStore loaded;
    bool success = true;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* word = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
        if (!word || !loaded.Add({word,
                                  static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1)),
                                  static_cast<std::uint64_t>(sqlite3_column_int64(statement, 2)),
                                  static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3))})) {
            success = false;
            break;
        }
    }
    success = success && stepResult == SQLITE_DONE;
    sqlite3_finalize(statement);
    statement = nullptr;
    if (success) {
        constexpr const char* preferenceQuery =
            "SELECT raw_text, candidate, alpha, beta, exposure, direct_event_mass "
            "FROM user_learning_preferences ORDER BY raw_text, candidate;";
        if (sqlite3_prepare_v2(database_, preferenceQuery, -1, &statement, nullptr) != SQLITE_OK) {
            return false;
        }
        while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto* rawText = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
            const auto* candidate = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 1));
            if (!rawText || !candidate ||
                !loaded.AddPreference({rawText, candidate,
                                       sqlite3_column_double(statement, 2),
                                       sqlite3_column_double(statement, 3),
                                       static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4)),
                                       sqlite3_column_double(statement, 5)})) {
                success = false;
                break;
            }
        }
        success = success && stepResult == SQLITE_DONE;
        sqlite3_finalize(statement);
    }
    if (success) learning = std::move(loaded);
    return success;
}

bool SqliteUserDictionaryRepository::SaveLearning(const UserLearningStore& learning) noexcept {
    if (!database_) return false;
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    bool success = sqlite3_exec(database_,
                                 "DELETE FROM user_learning;"
                                 "DELETE FROM user_learning_preferences;",
                                 nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "INSERT INTO user_learning (word, candidate_selections, raw_keeps, undo_count) VALUES (?, ?, ?, ?);";
    if (success && sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) success = false;
    if (success) {
        for (const auto& entry : learning.Entries()) {
            success = BindText16(statement, 1, entry.word) &&
                      sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(entry.candidateSelections)) == SQLITE_OK &&
                      sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(entry.rawKeeps)) == SQLITE_OK &&
                      sqlite3_bind_int64(statement, 4, static_cast<sqlite3_int64>(entry.undoCount)) == SQLITE_OK &&
                      sqlite3_step(statement) == SQLITE_DONE;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if (!success) break;
        }
    }
    if (statement) sqlite3_finalize(statement);
    statement = nullptr;
    if (success) {
        constexpr const char* preferenceQuery =
            "INSERT INTO user_learning_preferences "
            "(raw_text, candidate, alpha, beta, exposure, direct_event_mass) "
            "VALUES (?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(database_, preferenceQuery, -1, &statement, nullptr) != SQLITE_OK) {
            success = false;
        }
        if (success) {
            for (const auto& entry : learning.Preferences()) {
                success = BindText16(statement, 1, entry.rawText) &&
                          BindText16(statement, 2, entry.candidate) &&
                          sqlite3_bind_double(statement, 3, entry.alpha) == SQLITE_OK &&
                          sqlite3_bind_double(statement, 4, entry.beta) == SQLITE_OK &&
                          sqlite3_bind_int64(statement, 5,
                                             static_cast<sqlite3_int64>(entry.exposure)) == SQLITE_OK &&
                          sqlite3_bind_double(statement, 6, entry.directEventMass) == SQLITE_OK &&
                          sqlite3_step(statement) == SQLITE_DONE;
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                if (!success) break;
            }
        }
        if (statement) sqlite3_finalize(statement);
    }
    const char* transaction = success ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(database_, transaction, nullptr, nullptr, nullptr) != SQLITE_OK) success = false;
    return success;
}

bool SqliteUserDictionaryRepository::ResetLearning() noexcept {
    return database_ && sqlite3_exec(database_,
                                     "DELETE FROM user_learning;"
                                     "DELETE FROM user_learning_preferences;",
                                     nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool SqliteUserDictionaryRepository::LoadSocialLearning(
    SocialLearningStore& learning) const noexcept {
    if (!database_) return false;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "SELECT trigger, candidate, alpha, beta, exposure, direct_event_mass "
        "FROM social_learning ORDER BY trigger, candidate;";
    if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) return false;
    SocialLearningStore loaded;
    bool success = true;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* trigger = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
        const auto* candidate = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 1));
        if (!trigger || !candidate || !loaded.Add({trigger, candidate,
                                                    sqlite3_column_double(statement, 2),
                                                    sqlite3_column_double(statement, 3),
                                                    static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4)),
                                                    sqlite3_column_double(statement, 5)})) {
            success = false;
            break;
        }
    }
    success = success && stepResult == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (success) learning = std::move(loaded);
    return success;
}

bool SqliteUserDictionaryRepository::SaveSocialLearning(
    const SocialLearningStore& learning) noexcept {
    if (!database_) return false;
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    bool success = sqlite3_exec(database_, "DELETE FROM social_learning;", nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "INSERT INTO social_learning "
        "(trigger, candidate, alpha, beta, exposure, direct_event_mass) VALUES (?, ?, ?, ?, ?, ?);";
    if (success && sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) success = false;
    if (success) {
        for (const auto& entry : learning.Entries()) {
            success = BindText16(statement, 1, entry.trigger) && BindText16(statement, 2, entry.candidate) &&
                      sqlite3_bind_double(statement, 3, entry.alpha) == SQLITE_OK &&
                      sqlite3_bind_double(statement, 4, entry.beta) == SQLITE_OK &&
                      sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(entry.exposure)) == SQLITE_OK &&
                      sqlite3_bind_double(statement, 6, entry.directEventMass) == SQLITE_OK &&
                      sqlite3_step(statement) == SQLITE_DONE;
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if (!success) break;
        }
    }
    if (statement) sqlite3_finalize(statement);
    const char* transaction = success ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(database_, transaction, nullptr, nullptr, nullptr) != SQLITE_OK) success = false;
    return success;
}

bool SqliteUserDictionaryRepository::LoadJapaneseLearning(
    japanese::JapaneseLearningStore& learning) const noexcept {
    if (!database_) return false;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "SELECT reading, surface, selections, rejections, last_used FROM japanese_learning;";
    if (sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) return false;
    japanese::JapaneseLearningStore loaded;
    bool success = true;
    int stepResult = SQLITE_OK;
    try {
        while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto* reading = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 0));
            const auto* surface = static_cast<const wchar_t*>(sqlite3_column_text16(statement, 1));
            if (!reading || !surface ||
                !loaded.Add({reading, surface, sqlite3_column_double(statement, 2),
                             sqlite3_column_double(statement, 3),
                             static_cast<std::uint64_t>(sqlite3_column_int64(statement, 4))})) {
                success = false;
                break;
            }
        }
    } catch (...) {
        success = false;
    }
    success = success && stepResult == SQLITE_DONE;
    sqlite3_finalize(statement);
    if (success) learning = std::move(loaded);
    return success;
}

bool SqliteUserDictionaryRepository::SaveJapaneseLearning(
    const japanese::JapaneseLearningStore& learning) noexcept {
    if (!database_) return false;
    if (sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return false;
    bool success =
        sqlite3_exec(database_, "DELETE FROM japanese_learning;", nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    constexpr const char* query =
        "INSERT INTO japanese_learning (reading, surface, selections, rejections, last_used) "
        "VALUES (?, ?, ?, ?, ?);";
    if (success && sqlite3_prepare_v2(database_, query, -1, &statement, nullptr) != SQLITE_OK) success = false;
    try {
        if (success) {
            for (const auto& entry : learning.Entries()) {
                success = BindText16(statement, 1, entry.reading) && BindText16(statement, 2, entry.surface) &&
                          sqlite3_bind_double(statement, 3, entry.selections) == SQLITE_OK &&
                          sqlite3_bind_double(statement, 4, entry.rejections) == SQLITE_OK &&
                          sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(entry.lastUsed)) == SQLITE_OK &&
                          sqlite3_step(statement) == SQLITE_DONE;
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                if (!success) break;
            }
        }
    } catch (...) {
        success = false;
    }
    if (statement) sqlite3_finalize(statement);
    const char* transaction = success ? "COMMIT;" : "ROLLBACK;";
    if (sqlite3_exec(database_, transaction, nullptr, nullptr, nullptr) != SQLITE_OK) success = false;
    return success;
}

bool SqliteUserDictionaryRepository::ResetJapaneseLearning() noexcept {
    return database_ &&
           sqlite3_exec(database_, "DELETE FROM japanese_learning;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool SqliteUserDictionaryRepository::ResetSocialLearning() noexcept {
    return database_ && sqlite3_exec(database_, "DELETE FROM social_learning;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool SqliteUserDictionaryRepository::IsOpen() const noexcept {
    return database_ != nullptr;
}

bool SqliteUserDictionaryRepository::EnsureSchema() noexcept {
    return database_ &&
           sqlite3_exec(database_, kCreateSchema, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::filesystem::path DefaultUserDatabasePath() {
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (!localAppData || *localAppData == L'\0') return {};
    return std::filesystem::path(localAppData) / L"TEKITO" / L"user.db";
}

std::unique_ptr<IUserDataRepository> CreateDefaultUserDataRepository() noexcept {
    const auto path = DefaultUserDatabasePath();
    if (path.empty()) return nullptr;
    return std::unique_ptr<IUserDataRepository>(
        new (std::nothrow) SqliteUserDictionaryRepository(path));
}

}  // namespace tekito::userdata
