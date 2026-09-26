#pragma once

#include "Core/UserDictionary.h"

#include <filesystem>

namespace tekito::userdata {

[[nodiscard]] bool ExportUserDictionary(const UserDictionary& dictionary,
                                         const std::filesystem::path& path) noexcept;
[[nodiscard]] bool ImportUserDictionary(const std::filesystem::path& path,
                                         UserDictionary& dictionary) noexcept;

}  // namespace tekito::userdata
