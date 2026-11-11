#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::userdata {

// The optional AI model packs (japanese::AiModel), which Settings
// downloads: a zip of the pack's folder, checked against the SHA-256
// Settings was built with.

enum class AiModelInstallError { None, Download, Checksum, Unpack, Invalid, InUse };

// Unpacks `zip` (whose SHA-256 must be `sha256`, upper-case hex) and puts
// the pack `packId` in it into `dataRoot`, in place of an older copy.
[[nodiscard]] AiModelInstallError InstallAiModelPack(const std::filesystem::path& zip, std::string_view sha256,
                                                     std::wstring_view packId,
                                                     const std::filesystem::path& dataRoot);

// Unpacks `zip` (whose SHA-256 must be `sha256`) and puts each of the packs
// `packIds` in it into `dataRoot`, in place of older copies (the Japanese
// data, which Settings adds when TEKITO was installed without it).
[[nodiscard]] AiModelInstallError InstallDataPacks(const std::filesystem::path& zip, std::string_view sha256,
                                                   const std::vector<std::wstring>& packIds,
                                                   const std::filesystem::path& dataRoot);

// Takes the pack out of `dataRoot`; false when it could not be removed.
[[nodiscard]] bool RemoveAiModelPack(std::wstring_view packId, const std::filesystem::path& dataRoot);

// Whether a valid copy of the pack is in `dataRoot`.
[[nodiscard]] bool AiModelPackInstalled(std::wstring_view packId, const std::filesystem::path& dataRoot);
// Whether what runs the models (the ai-runtime pack, speaking this
// build's protocol) is in `dataRoot`.
[[nodiscard]] bool AiRuntimeInstalled(const std::filesystem::path& dataRoot);
// Whether that copy has Windows ML (to run them on a GPU or an NPU); the
// processor's copy has ONNX Runtime alone.
[[nodiscard]] bool AiRuntimeHasWindowsMl(const std::filesystem::path& dataRoot);

// Has TekitoAI.exe (next to this program) let go of its model files: it
// exits, and starts again when the input method next asks for it.
void StopAiHelper();

}  // namespace tekito::userdata
