#pragma once

#include "Core/InputMode.h"

#include <cstdint>
#include <memory>

namespace tekito::userdata {

class RuntimeModeState final {
public:
    explicit RuntimeModeState(InputMode fallbackMode,
                              const wchar_t* mappingName = L"Local\\TEKITO.RuntimeState.V2") noexcept;
    ~RuntimeModeState();

    RuntimeModeState(const RuntimeModeState&) = delete;
    RuntimeModeState& operator=(const RuntimeModeState&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept;
    [[nodiscard]] InputMode Mode() const noexcept;
    [[nodiscard]] std::uint32_t ModeGeneration() const noexcept;
    [[nodiscard]] std::uint32_t SettingsGeneration() const noexcept;
    [[nodiscard]] std::uint32_t DictionaryGeneration() const noexcept;
    [[nodiscard]] std::uint32_t LearningGeneration() const noexcept;
    void SetMode(InputMode mode) noexcept;
    void NotifySettingsChanged() noexcept;
    void NotifyDictionaryChanged() noexcept;
    void NotifyLearningChanged() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    InputMode fallbackMode_{InputMode::Convert};
};

}  // namespace tekito::userdata
