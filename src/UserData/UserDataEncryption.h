#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;

namespace tekito::userdata {

// Encryption of the user's database (user.db), when the user turns it on.
//
// One random 256-bit key encrypts the database's pages (SQLite3 Multiple
// Ciphers, ChaCha20-Poly1305). The key is kept in user.key beside it, twice
// wrapped: by the operating system for this user (Windows: DPAPI), which
// opens it day to day, and by a recovery code the user keeps (Argon2id, then
// XChaCha20-Poly1305), which opens it anywhere: on another PC, after Windows
// is reinstalled, or on another operating system.
//
// Every process with TEKITO open has the database open, so it changes
// between plain and encrypted only when no other process has it: the first
// to open it then (after the next sign-in, say) makes the change.

using DataKey = std::array<std::uint8_t, 32>;

// user.key beside the database.
[[nodiscard]] std::filesystem::path UserKeyPath(const std::filesystem::path& database);

// Whether the user asked for the database to be encrypted (user.key says so).
[[nodiscard]] bool EncryptionWanted(const std::filesystem::path& database);
// Whether the database file is encrypted now (it does not start as SQLite's
// own files do). False when there is no file.
[[nodiscard]] bool DatabaseEncrypted(const std::filesystem::path& database);

// Turns encryption on: makes the key when there is none, and a new recovery
// code (an earlier one no longer opens it). Returns the code (24 letters and
// digits in groups of four), or nothing when it could not be saved.
[[nodiscard]] std::optional<std::string> EnableEncryption(const std::filesystem::path& database);
// Turns it off: the database goes back to plain when it next can.
[[nodiscard]] bool DisableEncryption(const std::filesystem::path& database);
// A new recovery code for the key (the old one no longer opens it).
[[nodiscard]] std::optional<std::string> RenewRecoveryCode(const std::filesystem::path& database);

// Before opening: makes the database what user.key asks (encrypted or
// plain) when no other process has it open. True when it is as asked.
bool SettleEncryption(const std::filesystem::path& database) noexcept;
// After opening an encrypted database: gives the connection its key (the
// one the system keeps for this user). False when it cannot be had.
[[nodiscard]] bool UnlockDatabase(sqlite3* connection, const std::filesystem::path& database) noexcept;

// Rescue: with the recovery code, writes everything readable from the
// database (and its key file) to `plain`, a database without encryption.
// The tables that cannot be read whole are copied as far as they can be.
// Returns false when the code does not open the key.
[[nodiscard]] bool RescueDatabase(const std::filesystem::path& database, const std::filesystem::path& keyFile,
                                  std::string_view recoveryCode, const std::filesystem::path& plain);

}  // namespace tekito::userdata
