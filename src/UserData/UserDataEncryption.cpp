#include "UserData/UserDataEncryption.h"

#include "monocypher.h"
#include "sqlite3mc_amalgamation.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace tekito::userdata {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr char kHeader[] = "TEKITO user key 1";
// Recovery codes: Crockford's base32 (no I, L, O, U), 24 symbols = 120 bits.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t kCodeSymbols = 24;
// Argon2id for the recovery code: 64 MiB, 3 passes (well under a second).
constexpr std::uint32_t kArgonBlocks = 65536;
constexpr std::uint32_t kArgonPasses = 3;

// What user.key holds.
struct KeyFile {
    bool encrypt{false};
    Bytes system;  // DPAPI's wrapping of the key
    std::uint32_t blocks{kArgonBlocks}, passes{kArgonPasses};
    std::array<std::uint8_t, 16> salt{};
    std::array<std::uint8_t, 24> nonce{};
    std::array<std::uint8_t, 16> mac{};
    DataKey wrapped{};
    bool hasRecovery{false};
};

std::string Base64(const std::uint8_t* data, std::size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t n = (data[i] << 16) | (i + 1 < size ? data[i + 1] << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < size ? kTable[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kTable[n & 63] : '=';
    }
    return out;
}

std::optional<Bytes> Unbase64(std::string_view text) {
    Bytes out;
    std::uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return std::nullopt;
        bits = (bits << 6) | static_cast<std::uint32_t>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>((bits >> count) & 0xFF));
        }
    }
    return out;
}

template <std::size_t N>
bool Fill(std::array<std::uint8_t, N>& to, const std::optional<Bytes>& from) {
    if (!from || from->size() != N) return false;
    std::copy(from->begin(), from->end(), to.begin());
    return true;
}

bool RandomBytes(std::uint8_t* data, std::size_t size) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, data, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::optional<KeyFile> ReadKeyFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind(kHeader, 0) != 0) return std::nullopt;
    KeyFile file;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string name;
        fields >> name;
        if (name == "encrypt") {
            int on = 0;
            fields >> on;
            file.encrypt = on != 0;
        } else if (name == "system") {
            std::string kind, data;
            fields >> kind >> data;
            if (kind != "dpapi") continue;
            if (auto bytes = Unbase64(data)) file.system = std::move(*bytes);
        } else if (name == "recovery") {
            std::string kind, salt, nonce, mac, wrapped;
            fields >> kind >> file.blocks >> file.passes >> salt >> nonce >> mac >> wrapped;
            file.hasRecovery = kind == "argon2id" && Fill(file.salt, Unbase64(salt)) && Fill(file.nonce, Unbase64(nonce)) &&
                               Fill(file.mac, Unbase64(mac)) && Fill(file.wrapped, Unbase64(wrapped));
        }
    }
    return file;
}

bool WriteKeyFile(const std::filesystem::path& path, const KeyFile& file) {
    std::ostringstream out;
    out << kHeader << "\n";
    out << "encrypt " << (file.encrypt ? 1 : 0) << "\n";
    if (!file.system.empty()) out << "system dpapi " << Base64(file.system.data(), file.system.size()) << "\n";
    if (file.hasRecovery) {
        out << "recovery argon2id " << file.blocks << " " << file.passes << " " << Base64(file.salt.data(), file.salt.size())
            << " " << Base64(file.nonce.data(), file.nonce.size()) << " " << Base64(file.mac.data(), file.mac.size()) << " "
            << Base64(file.wrapped.data(), file.wrapped.size()) << "\n";
    }
    // Written beside it, then put in place, so a failure leaves the old one.
    auto temporary = path;
    temporary += L".new";
    {
        std::ofstream file_(temporary, std::ios::binary | std::ios::trunc);
        const auto text = out.str();
        file_.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file_) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::optional<Bytes> SystemWrap(const DataKey& key) {
    DATA_BLOB in{static_cast<DWORD>(key.size()), const_cast<BYTE*>(key.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"TEKITO user key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    Bytes wrapped(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return wrapped;
}

std::optional<DataKey> SystemUnwrap(const KeyFile& file) {
    if (file.system.empty()) return std::nullopt;
    DATA_BLOB in{static_cast<DWORD>(file.system.size()), const_cast<BYTE*>(file.system.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    std::optional<DataKey> key;
    if (out.cbData == DataKey{}.size()) {
        key.emplace();
        std::memcpy(key->data(), out.pbData, out.cbData);
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return key;
}

// The code as typed: letters in either case, dashes and spaces ignored, O as
// 0 and I or L as 1. Empty when it is not a code.
std::string NormalizeCode(std::string_view code) {
    std::string out;
    for (char c : code) {
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (!std::strchr(kAlphabet, c) || c == '\0') return {};
        out += c;
    }
    return out.size() == kCodeSymbols ? out : std::string();
}

// The key that wraps the data key, from the code (Argon2id).
std::optional<DataKey> CodeKey(const std::string& code, const KeyFile& file) {
    if (file.blocks < 8 || file.blocks > (1u << 20) || file.passes < 1 || file.passes > 16) return std::nullopt;
    std::unique_ptr<std::uint8_t[]> work(new (std::nothrow) std::uint8_t[static_cast<std::size_t>(file.blocks) * 1024]);
    if (!work) return std::nullopt;
    DataKey key{};
    const crypto_argon2_config config{CRYPTO_ARGON2_ID, file.blocks, file.passes, 1};
    const crypto_argon2_inputs inputs{reinterpret_cast<const std::uint8_t*>(code.data()), file.salt.data(),
                                      static_cast<std::uint32_t>(code.size()), static_cast<std::uint32_t>(file.salt.size())};
    crypto_argon2(key.data(), static_cast<std::uint32_t>(key.size()), work.get(), config, inputs, crypto_argon2_no_extras);
    return key;
}

// A new code, the data key wrapped with it into `file`.
std::optional<std::string> WrapWithNewCode(const DataKey& key, KeyFile& file) {
    std::array<std::uint8_t, kCodeSymbols> picks{};
    if (!RandomBytes(picks.data(), picks.size()) || !RandomBytes(file.salt.data(), file.salt.size()) ||
        !RandomBytes(file.nonce.data(), file.nonce.size())) {
        return std::nullopt;
    }
    std::string code;
    for (const auto pick : picks) code += kAlphabet[pick & 31];
    file.blocks = kArgonBlocks;
    file.passes = kArgonPasses;
    auto codeKey = CodeKey(code, file);
    if (!codeKey) return std::nullopt;
    crypto_aead_lock(file.wrapped.data(), file.mac.data(), codeKey->data(), file.nonce.data(), nullptr, 0, key.data(),
                     key.size());
    crypto_wipe(codeKey->data(), codeKey->size());
    file.hasRecovery = true;
    std::string grouped;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (i && i % 4 == 0) grouped += '-';
        grouped += code[i];
    }
    return grouped;
}

std::optional<DataKey> CodeUnwrap(const KeyFile& file, std::string_view code) {
    const auto normal = NormalizeCode(code);
    if (normal.empty() || !file.hasRecovery) return std::nullopt;
    auto codeKey = CodeKey(normal, file);
    if (!codeKey) return std::nullopt;
    DataKey key{};
    const bool ok = crypto_aead_unlock(key.data(), file.mac.data(), codeKey->data(), file.nonce.data(), nullptr, 0,
                                       file.wrapped.data(), file.wrapped.size()) == 0;
    crypto_wipe(codeKey->data(), codeKey->size());
    if (!ok) return std::nullopt;
    return key;
}

// sqlite3mc's raw key: "raw:" and the 32 bytes (no key derivation).
std::array<std::uint8_t, 36> RawKey(const DataKey& key) {
    std::array<std::uint8_t, 36> raw{'r', 'a', 'w', ':'};
    std::copy(key.begin(), key.end(), raw.begin() + 4);
    return raw;
}

bool KeyConnection(sqlite3* connection, const DataKey& key) {
    auto raw = RawKey(key);
    const bool ok = sqlite3_key_v2(connection, "main", raw.data(), static_cast<int>(raw.size())) == SQLITE_OK;
    SecureZeroMemory(raw.data(), raw.size());
    return ok && sqlite3_exec(connection, "SELECT count(*) FROM sqlite_master;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

// No other process has the file open (each process with TEKITO has it).
bool NobodyElseHasIt(const std::filesystem::path& database) {
    const HANDLE file = CreateFileW(database.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

std::string JournalMode(sqlite3* connection, const char* pragma) {
    std::string mode;
    sqlite3_exec(
        connection, pragma,
        [](void* out, int, char** values, char**) {
            if (values && values[0]) *static_cast<std::string*>(out) = values[0];
            return 0;
        },
        &mode, nullptr);
    return mode;
}

}  // namespace

std::filesystem::path UserKeyPath(const std::filesystem::path& database) {
    auto path = database;
    return path.replace_extension(L".key");
}

bool EncryptionWanted(const std::filesystem::path& database) {
    const auto file = ReadKeyFile(UserKeyPath(database));
    return file && file->encrypt;
}

bool DatabaseEncrypted(const std::filesystem::path& database) {
    std::ifstream in(database, std::ios::binary);
    char header[16]{};
    if (!in.read(header, sizeof header)) return false;
    return std::memcmp(header, "SQLite format 3", 16) != 0;
}

std::optional<std::string> EnableEncryption(const std::filesystem::path& database) {
    const auto path = UserKeyPath(database);
    auto file = ReadKeyFile(path).value_or(KeyFile{});
    std::optional<DataKey> key = SystemUnwrap(file);
    if (!key) {
        // A key of its own (none yet, or one this user's system cannot open
        // and the database is not encrypted with).
        if (DatabaseEncrypted(database)) return std::nullopt;
        key.emplace();
        if (!RandomBytes(key->data(), key->size())) return std::nullopt;
        auto system = SystemWrap(*key);
        if (!system) return std::nullopt;
        file.system = std::move(*system);
    }
    file.encrypt = true;
    auto code = WrapWithNewCode(*key, file);
    crypto_wipe(key->data(), key->size());
    if (!code || !WriteKeyFile(path, file)) return std::nullopt;
    SettleEncryption(database);
    return code;
}

bool DisableEncryption(const std::filesystem::path& database) {
    const auto path = UserKeyPath(database);
    auto file = ReadKeyFile(path);
    if (!file) return true;
    file->encrypt = false;
    if (!WriteKeyFile(path, *file)) return false;
    SettleEncryption(database);
    return true;
}

std::optional<std::string> RenewRecoveryCode(const std::filesystem::path& database) {
    const auto path = UserKeyPath(database);
    auto file = ReadKeyFile(path);
    if (!file) return std::nullopt;
    auto key = SystemUnwrap(*file);
    if (!key) return std::nullopt;
    auto code = WrapWithNewCode(*key, *file);
    crypto_wipe(key->data(), key->size());
    if (!code || !WriteKeyFile(path, *file)) return std::nullopt;
    return code;
}

bool SettleEncryption(const std::filesystem::path& database) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(database, error) || std::filesystem::file_size(database, error) == 0) return true;
        const auto file = ReadKeyFile(UserKeyPath(database));
        const bool want = file && file->encrypt;
        const bool encrypted = DatabaseEncrypted(database);
        if (want == encrypted) return true;
        if (!file || !NobodyElseHasIt(database)) return false;
        auto key = SystemUnwrap(*file);
        if (!key) return false;
        sqlite3* connection = nullptr;
        bool done = false;
        if (sqlite3_open16(database.c_str(), &connection) == SQLITE_OK && (!encrypted || KeyConnection(connection, *key))) {
            // The log folded in, and a rollback journal while the pages are
            // rewritten (the change is undone whole if it stops).
            sqlite3_exec(connection, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
            if (JournalMode(connection, "PRAGMA journal_mode=DELETE;") == "delete") {
                auto raw = RawKey(*key);
                const int rc = want ? sqlite3_rekey(connection, raw.data(), static_cast<int>(raw.size()))
                                    : sqlite3_rekey(connection, nullptr, 0);
                SecureZeroMemory(raw.data(), raw.size());
                done = rc == SQLITE_OK;
                JournalMode(connection, "PRAGMA journal_mode=WAL;");
            }
        }
        if (connection) sqlite3_close(connection);
        crypto_wipe(key->data(), key->size());
        return done;
    } catch (...) {
        return false;
    }
}

bool UnlockDatabase(sqlite3* connection, const std::filesystem::path& database) noexcept {
    try {
        const auto file = ReadKeyFile(UserKeyPath(database));
        if (!file) return false;
        auto key = SystemUnwrap(*file);
        if (!key) return false;
        const bool ok = KeyConnection(connection, *key);
        crypto_wipe(key->data(), key->size());
        return ok;
    } catch (...) {
        return false;
    }
}

bool RescueDatabase(const std::filesystem::path& database, const std::filesystem::path& keyFile,
                    std::string_view recoveryCode, const std::filesystem::path& plain) {
    const auto file = ReadKeyFile(keyFile);
    if (!file) return false;
    auto key = CodeUnwrap(*file, recoveryCode);
    if (!key) return false;
    // Read from a copy (with its log), never the original.
    std::error_code error;
    wchar_t folder[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder);
    const auto copy = std::filesystem::path(folder) / (L"tekito-rescue-" + std::to_wstring(GetCurrentProcessId()) + L".db");
    std::filesystem::copy_file(database, copy, std::filesystem::copy_options::overwrite_existing, error);
    auto wal = database;
    wal += L"-wal";
    auto copyWal = copy;
    copyWal += L"-wal";
    if (std::filesystem::is_regular_file(wal, error)) {
        std::filesystem::copy_file(wal, copyWal, std::filesystem::copy_options::overwrite_existing, error);
    }
    std::filesystem::remove(plain, error);
    sqlite3* source = nullptr;
    sqlite3* target = nullptr;
    bool opened = sqlite3_open16(copy.c_str(), &source) == SQLITE_OK && KeyConnection(source, *key) &&
                  sqlite3_open16(plain.c_str(), &target) == SQLITE_OK;
    crypto_wipe(key->data(), key->size());
    if (opened) {
        // Each table: its schema, then its rows, as many as can be read.
        sqlite3_stmt* tables = nullptr;
        sqlite3_prepare_v2(source, "SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%';",
                           -1, &tables, nullptr);
        while (tables && sqlite3_step(tables) == SQLITE_ROW) {
            const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(tables, 0));
            const auto* sql = reinterpret_cast<const char*>(sqlite3_column_text(tables, 1));
            if (!sql || sqlite3_exec(target, sql, nullptr, nullptr, nullptr) != SQLITE_OK) continue;
            sqlite3_stmt* rows = nullptr;
            if (sqlite3_prepare_v2(source, ("SELECT * FROM \"" + name + "\";").c_str(), -1, &rows, nullptr) != SQLITE_OK) continue;
            const int columns = sqlite3_column_count(rows);
            std::string insert = "INSERT OR IGNORE INTO \"" + name + "\" VALUES (";
            for (int c = 0; c < columns; ++c) insert += c ? ",?" : "?";
            insert += ");";
            sqlite3_stmt* put = nullptr;
            sqlite3_prepare_v2(target, insert.c_str(), -1, &put, nullptr);
            sqlite3_exec(target, "BEGIN;", nullptr, nullptr, nullptr);
            while (put && sqlite3_step(rows) == SQLITE_ROW) {
                for (int c = 0; c < columns; ++c) sqlite3_bind_value(put, c + 1, sqlite3_column_value(rows, c));
                sqlite3_step(put);
                sqlite3_reset(put);
            }
            sqlite3_exec(target, "COMMIT;", nullptr, nullptr, nullptr);
            sqlite3_finalize(put);
            sqlite3_finalize(rows);
        }
        sqlite3_finalize(tables);
    }
    if (source) sqlite3_close(source);
    if (target) sqlite3_close(target);
    std::filesystem::remove(copy, error);
    std::filesystem::remove(copyWal, error);
    auto shm = copy;
    shm += L"-shm";
    std::filesystem::remove(shm, error);
    return opened;
}

}  // namespace tekito::userdata
