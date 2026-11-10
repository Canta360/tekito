#include "UserData/AiModelInstall.h"

#include "Core/Japanese/AiJudge.h"
#include "UserData/DataPackValidation.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace tekito::userdata {
namespace {

// Unpacks `zip` into `into` with Windows' own tar.
bool Unpack(const std::filesystem::path& zip, const std::filesystem::path& into) {
    std::error_code ignored;
    std::filesystem::remove_all(into, ignored);
    std::filesystem::create_directories(into, ignored);
    wchar_t system[MAX_PATH]{};
    if (GetSystemDirectoryW(system, MAX_PATH) == 0) return false;
    std::wstring command = L"\"" + (std::filesystem::path(system) / L"tar.exe").wstring() + L"\" -xf \"" +
                           zip.wstring() + L"\" -C \"" + into.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        return false;
    }
    constexpr DWORD kUnpackTimeout = 10 * 60 * 1000;
    const bool finished = WaitForSingleObject(process.hProcess, kUnpackTimeout) == WAIT_OBJECT_0;
    DWORD exitCode = 1;
    if (!finished) TerminateProcess(process.hProcess, 1);
    else GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return finished && exitCode == 0;
}

// Removes `directory`, trying again for a moment while the helper that
// was told to stop still has its model mapped.
bool RemoveWhenFree(const std::filesystem::path& directory) {
    std::error_code error;
    for (int tries = 0; tries < 20; ++tries) {
        // What the NPU's compiler keeps (npu-cache) is read-only.
        for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(it->path().c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
            }
        }
        error.clear();
        std::filesystem::remove_all(directory, error);
        if (!error || !std::filesystem::exists(directory, error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

}  // namespace

AiModelInstallError InstallAiModelPack(const std::filesystem::path& zip, std::string_view sha256,
                                       std::wstring_view packId, const std::filesystem::path& dataRoot) {
    if (FileSha256(zip) != sha256) return AiModelInstallError::Checksum;
    std::error_code error;
    const auto unpacked = dataRoot / (std::wstring(packId) + L".download");
    if (!Unpack(zip, unpacked)) {
        std::filesystem::remove_all(unpacked, error);
        return AiModelInstallError::Unpack;
    }
    const auto pack = unpacked / std::wstring(packId);
    DataPackStatus status;
    if (!ValidateDataPack(pack, status) || status.packId != packId) {
        std::filesystem::remove_all(unpacked, error);
        return AiModelInstallError::Invalid;
    }
    const auto target = dataRoot / std::wstring(packId);
    StopAiHelper();
    if (std::filesystem::exists(target, error) && !RemoveWhenFree(target)) {
        std::filesystem::remove_all(unpacked, error);
        return AiModelInstallError::InUse;
    }
    std::filesystem::rename(pack, target, error);
    std::error_code ignored;
    std::filesystem::remove_all(unpacked, ignored);
    return error ? AiModelInstallError::Unpack : AiModelInstallError::None;
}

bool RemoveAiModelPack(std::wstring_view packId, const std::filesystem::path& dataRoot) {
    const auto target = dataRoot / std::wstring(packId);
    std::error_code error;
    if (!std::filesystem::exists(target, error)) return true;
    StopAiHelper();
    return RemoveWhenFree(target);
}

bool AiModelPackInstalled(std::wstring_view packId, const std::filesystem::path& dataRoot) {
    std::error_code error;
    const auto pack = dataRoot / std::wstring(packId);
    return std::filesystem::is_regular_file(pack / L"manifest.json", error) &&
           std::filesystem::is_regular_file(pack / L"model.onnx", error);
}

bool AiRuntimeInstalled(const std::filesystem::path& dataRoot) {
    std::error_code error;
    const auto pack = dataRoot / std::wstring(japanese::aihelper::kRuntimePack);
    // "protocol": "TKA3" in its manifest (an older pack ran llama.cpp).
    std::ifstream in(pack / L"manifest.json", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = manifest.find("\"protocol\"");
    const auto value = key == std::string::npos ? key : manifest.find('"', manifest.find(':', key) + 1);
    if (value == std::string::npos ||
        manifest.compare(value + 1, japanese::aihelper::kProtocol.size(), japanese::aihelper::kProtocol) != 0) {
        return false;
    }
    return std::filesystem::is_regular_file(pack / L"manifest.json", error) &&
           std::filesystem::is_regular_file(pack / L"TekitoAI.exe", error);
}

bool AiRuntimeHasWindowsMl(const std::filesystem::path& dataRoot) {
    std::error_code error;
    const auto pack = dataRoot / std::wstring(japanese::aihelper::kRuntimePack);
    return AiRuntimeInstalled(dataRoot) &&
           std::filesystem::is_regular_file(pack / L"Microsoft.Windows.AI.MachineLearning.dll", error);
}

void StopAiHelper() { japanese::aihelper::Stop(); }

}  // namespace tekito::userdata
