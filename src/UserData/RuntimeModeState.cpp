#include "UserData/RuntimeModeState.h"

#include <windows.h>

#include <new>

namespace tekito::userdata {
namespace {

constexpr LONG kMagic = 0x544B5254;
constexpr LONG kInitializing = 1;

struct SharedRuntimeState {
    volatile LONG magic;
    volatile LONG mode;
    volatile LONG modeGeneration;
    volatile LONG settingsGeneration;
    volatile LONG dictionaryGeneration;
    volatile LONG learningGeneration;
};

}  // namespace

struct RuntimeModeState::Impl {
    HANDLE mapping{};
    SharedRuntimeState* state{};
};

RuntimeModeState::RuntimeModeState(InputMode fallbackMode,
                                   const wchar_t* mappingName) noexcept
    : fallbackMode_(fallbackMode), impl_(new (std::nothrow) Impl) {
    if (!impl_ || !mappingName || !*mappingName) return;
    impl_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(SharedRuntimeState), mappingName);
    if (!impl_->mapping) return;
    impl_->state = static_cast<SharedRuntimeState*>(
        MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                      sizeof(SharedRuntimeState)));
    if (!impl_->state) return;

    if (InterlockedCompareExchange(&impl_->state->magic, kInitializing, 0) == 0) {
        InterlockedExchange(&impl_->state->mode, static_cast<LONG>(fallbackMode));
        InterlockedExchange(&impl_->state->modeGeneration, 1);
        InterlockedExchange(&impl_->state->settingsGeneration, 1);
        InterlockedExchange(&impl_->state->dictionaryGeneration, 1);
        InterlockedExchange(&impl_->state->learningGeneration, 1);
        InterlockedExchange(&impl_->state->magic, kMagic);
    } else {
        for (int attempt = 0; attempt < 100 && impl_->state->magic == kInitializing; ++attempt) {
            SwitchToThread();
        }
    }
}

RuntimeModeState::~RuntimeModeState() {
    if (!impl_) return;
    if (impl_->state) UnmapViewOfFile(impl_->state);
    if (impl_->mapping) CloseHandle(impl_->mapping);
}

bool RuntimeModeState::IsAvailable() const noexcept {
    return impl_ && impl_->state && impl_->state->magic == kMagic;
}

InputMode RuntimeModeState::Mode() const noexcept {
    if (!IsAvailable()) return fallbackMode_;
    return InterlockedCompareExchange(&impl_->state->mode, 0, 0) ==
                   static_cast<LONG>(InputMode::Direct)
               ? InputMode::Direct
               : InputMode::Convert;
}

std::uint32_t RuntimeModeState::ModeGeneration() const noexcept {
    return IsAvailable() ? static_cast<std::uint32_t>(
                               InterlockedCompareExchange(&impl_->state->modeGeneration, 0, 0))
                         : 0;
}

std::uint32_t RuntimeModeState::SettingsGeneration() const noexcept {
    return IsAvailable() ? static_cast<std::uint32_t>(
                               InterlockedCompareExchange(&impl_->state->settingsGeneration, 0, 0))
                         : 0;
}

std::uint32_t RuntimeModeState::DictionaryGeneration() const noexcept {
    return IsAvailable() ? static_cast<std::uint32_t>(
                               InterlockedCompareExchange(&impl_->state->dictionaryGeneration, 0, 0))
                         : 0;
}

std::uint32_t RuntimeModeState::LearningGeneration() const noexcept {
    return IsAvailable() ? static_cast<std::uint32_t>(
                               InterlockedCompareExchange(&impl_->state->learningGeneration, 0, 0))
                         : 0;
}

void RuntimeModeState::SetMode(InputMode mode) noexcept {
    if (!IsAvailable()) return;
    const LONG value = static_cast<LONG>(mode);
    if (InterlockedExchange(&impl_->state->mode, value) != value) {
        InterlockedIncrement(&impl_->state->modeGeneration);
    }
}

void RuntimeModeState::NotifySettingsChanged() noexcept {
    if (IsAvailable()) InterlockedIncrement(&impl_->state->settingsGeneration);
}

void RuntimeModeState::NotifyDictionaryChanged() noexcept {
    if (IsAvailable()) InterlockedIncrement(&impl_->state->dictionaryGeneration);
}

void RuntimeModeState::NotifyLearningChanged() noexcept {
    if (IsAvailable()) InterlockedIncrement(&impl_->state->learningGeneration);
}

}  // namespace tekito::userdata
