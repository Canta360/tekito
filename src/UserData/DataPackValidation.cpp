#include "UserData/DataPackValidation.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace tekito::userdata {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string JsonString(const std::string& json, const std::string& key,
                       std::size_t start = 0) {
    const auto keyPosition = json.find('"' + key + '"', start);
    if (keyPosition == std::string::npos) return {};
    const auto colon = json.find(':', keyPosition);
    const auto firstQuote = json.find('"', colon + 1);
    if (colon == std::string::npos || firstQuote == std::string::npos) return {};
    const auto secondQuote = json.find('"', firstQuote + 1);
    return secondQuote == std::string::npos ? std::string{} :
           json.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

std::string NestedJsonString(const std::string& json, const std::string& object,
                             const std::string& key) {
    const auto objectPosition = json.find('"' + object + '"');
    if (objectPosition == std::string::npos) return {};
    return JsonString(json, key, objectPosition);
}

std::wstring Wide(std::string value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char ch : value) result.push_back(static_cast<wchar_t>(ch));
    return result;
}

bool IsPackRelativePath(const std::filesystem::path& packPath,
                        const std::filesystem::path& relativePath) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(packPath, error);
    if (error) return false;
    const auto target = std::filesystem::weakly_canonical(packPath / relativePath, error);
    if (error) return false;
    const auto relative = std::filesystem::relative(target, root, error);
    if (error || relative.empty() || relative.is_absolute()) return false;
    const auto first = relative.begin();
    return first == relative.end() || *first != L"..";
}

std::string Sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD resultLength = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &resultLength, 0) != 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<BYTE> object(objectLength);
    std::vector<BYTE> digest(32);
    bool ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
                               nullptr, 0, 0) == 0;
    std::vector<BYTE> buffer(64 * 1024);
    while (ok && input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = input.gcount();
        if (bytesRead > 0) {
            ok = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(bytesRead), 0) == 0;
        }
    }
    ok = ok && !input.bad() &&
         BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (const BYTE value : digest) output << std::setw(2) << static_cast<unsigned int>(value);
    return output.str();
}

}  // namespace

bool ValidateDataPack(const std::filesystem::path& packPath,
                      DataPackStatus& status) noexcept {
    status = {};
    try {
        const auto manifestPath = packPath / L"manifest.json";
        const auto manifest = ReadFile(manifestPath);
        if (manifest.empty()) {
            status.reason = L"manifest missing or empty";
            return false;
        }
        const auto packId = JsonString(manifest, "pack_id").empty()
                                ? JsonString(manifest, "id")
                                : JsonString(manifest, "pack_id");
        const auto notice = JsonString(manifest, "notice_file");
        const auto dataFile = JsonString(manifest, "file");
        const auto indexFile = JsonString(manifest, "index_file");
        const auto fileHash = NestedJsonString(manifest, "sha256", "file");
        const auto indexHash = NestedJsonString(manifest, "sha256", "index");
        status.packId = Wide(packId);
        status.displayName = Wide(JsonString(manifest, "display_name"));
        status.version = Wide(JsonString(manifest, "version"));
        status.type = Wide(JsonString(manifest, "type"));
        status.source = Wide(JsonString(manifest, "source"));
        status.license = Wide(JsonString(manifest, "license"));
        status.packPath = packPath;
        // Packs read in place from one file (the Japanese meanings and
        // language model) have no index.
        const bool indexed = !indexFile.empty();
        if (packId.empty() || status.version.empty() || JsonString(manifest, "language").empty() ||
            JsonString(manifest, "type").empty() || JsonString(manifest, "license").empty() ||
            notice.empty() || dataFile.empty() || fileHash.empty() || (indexed && indexHash.empty())) {
            status.reason = L"required manifest metadata missing";
            return false;
        }
        const std::filesystem::path noticeRelative(notice);
        const std::filesystem::path dataRelative(dataFile);
        const std::filesystem::path indexRelative(indexed ? indexFile : dataFile);
        if (!IsPackRelativePath(packPath, noticeRelative) ||
            !IsPackRelativePath(packPath, dataRelative) ||
            !IsPackRelativePath(packPath, indexRelative)) {
            status.reason = L"manifest path escapes pack directory";
            return false;
        }
        const auto noticePath = packPath / noticeRelative;
        status.noticePath = noticePath;
        const auto dataPath = packPath / dataRelative;
        const auto indexPath = packPath / indexRelative;
        if (!std::filesystem::exists(noticePath) || !std::filesystem::exists(dataPath) ||
            !std::filesystem::exists(indexPath)) {
            status.reason = L"notice, data, or index file missing";
            return false;
        }
        status.fileChecksumValid = Sha256(dataPath) == fileHash;
        status.indexChecksumValid = !indexed || Sha256(indexPath) == indexHash;
        if (!status.fileChecksumValid || !status.indexChecksumValid) {
            status.reason = L"checksum mismatch";
            return false;
        }
        status.valid = true;
        status.reason = L"valid";
        return true;
    } catch (...) {
        status.reason = L"validation exception";
        return false;
    }
}

}  // namespace tekito::userdata
