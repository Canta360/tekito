#pragma once

#include "Core/InputMode.h"

#include <cstdint>
#include <memory>

namespace tekito::userdata {

// State every TEKITO client in the session shares: the English profile's
// mode, the Japanese profile's, and counters that tell clients to reload
// settings, the dictionary or learning. The fallbacks seed a new session.
class RuntimeModeState final {
public:
    explicit RuntimeModeState(InputMode fallbackMode,
                              const wchar_t* mappingName = L"Local\\TEKITO.RuntimeState.V3",
                              InputMode japaneseFallbackMode = InputMode::Japanese) noexcept;
    ~RuntimeModeState();

    RuntimeModeState(const RuntimeModeState&) = delete;
    RuntimeModeState& operator=(const RuntimeModeState&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept;
    // The en-US profile: Convert or Direct.
    [[nodiscard]] InputMode Mode() const noexcept;
    // The ja-JP profile: Japanese, Convert or Direct.
    [[nodiscard]] InputMode JapaneseMode() const noexcept;
    [[nodiscard]] std::uint32_t ModeGeneration() const noexcept;
    [[nodiscard]] std::uint32_t SettingsGeneration() const noexcept;
    [[nodiscard]] std::uint32_t DictionaryGeneration() const noexcept;
    [[nodiscard]] std::uint32_t LearningGeneration() const noexcept;
    void SetMode(InputMode mode) noexcept;
    void SetJapaneseMode(InputMode mode) noexcept;
    void NotifySettingsChanged() noexcept;
    void NotifyDictionaryChanged() noexcept;
    void NotifyLearningChanged() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    InputMode fallbackMode_{InputMode::Convert};
    InputMode japaneseFallbackMode_{InputMode::Japanese};
};

}  // namespace tekito::userdata
