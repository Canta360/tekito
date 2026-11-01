#pragma once

#include "Core/InputMode.h"

#include <cstdint>
#include <memory>

namespace tekito::userdata {

inline constexpr const wchar_t* kRuntimeStateName = L"Local\\TEKITO.RuntimeState.V4";

// State every TEKITO client in the session shares: the English profile's
// mode, the Japanese profile's, secret mode, and counters that tell clients
// to reload settings, the dictionary or learning. The fallbacks seed a new
// session.
class RuntimeModeState final {
public:
    explicit RuntimeModeState(InputMode fallbackMode,
                              const wchar_t* mappingName = kRuntimeStateName,
                              InputMode japaneseFallbackMode = InputMode::Japanese) noexcept;
    ~RuntimeModeState();

    RuntimeModeState(const RuntimeModeState&) = delete;
    RuntimeModeState& operator=(const RuntimeModeState&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept;
    // The en-US profile: Convert or Direct.
    [[nodiscard]] InputMode Mode() const noexcept;
    // The ja-JP profile: Japanese, Mixed, Convert or Direct.
    [[nodiscard]] InputMode JapaneseMode() const noexcept;
    [[nodiscard]] std::uint32_t ModeGeneration() const noexcept;
    [[nodiscard]] std::uint32_t SettingsGeneration() const noexcept;
    [[nodiscard]] std::uint32_t DictionaryGeneration() const noexcept;
    [[nodiscard]] std::uint32_t LearningGeneration() const noexcept;
    void SetMode(InputMode mode) noexcept;
    void SetJapaneseMode(InputMode mode) noexcept;
    // Secret mode, for the session only (never saved): nothing typed is
    // learned or kept, as in a private window, in every app.
    [[nodiscard]] bool Secret() const noexcept;
    void SetSecret(bool on) noexcept;
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
