#pragma once

#include <windows.h>

#include <cwchar>
#include <mutex>

namespace tekito::tsf {

inline bool TraceEnabled() noexcept {
#if defined(TEKITO_TSF_TRACE_DEFAULT)
    return true;
#else
    static const bool enabled = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"TEKITO_TSF_TRACE", value, 2) == 1 &&
               value[0] == L'1';
    }();
    return enabled;
#endif
}

struct TraceState {
    std::mutex mutex;
    HANDLE file{INVALID_HANDLE_VALUE};

    ~TraceState() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

inline TraceState& GetTraceState() noexcept {
    static TraceState state;
    return state;
}

inline void Trace(const wchar_t* message) noexcept {
    if (!TraceEnabled()) return;

    auto& state = GetTraceState();
    std::lock_guard lock(state.mutex);
    if (state.file == INVALID_HANDLE_VALUE) {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, path);
        if (length == 0 || length >= MAX_PATH || wcscat_s(path, L"TekitoTsf.log") != 0) {
            return;
        }
        state.file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        if (state.file != INVALID_HANDLE_VALUE && GetFileSizeEx(state.file, &size) &&
            size.QuadPart == 0) {
            constexpr wchar_t bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(state.file, &bom, sizeof(bom), &written, nullptr);
        }
    }
    if (state.file == INVALID_HANDLE_VALUE) return;

    wchar_t line[512]{};
    const DWORD pid = GetCurrentProcessId();
    swprintf_s(line, L"[%lu] %s\r\n", pid, message);
    DWORD written = 0;
    WriteFile(state.file, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    OutputDebugStringW(line);
}

inline void TraceHr(const wchar_t* message, HRESULT hr) noexcept {
    wchar_t line[256]{};
    swprintf_s(line, L"%s hr=0x%08lX", message, static_cast<unsigned long>(hr));
    Trace(line);
}

class ScopedTraceDuration final {
public:
    explicit ScopedTraceDuration(const wchar_t* label) noexcept
        : label_(label), enabled_(TraceEnabled()) {
        if (enabled_) QueryPerformanceCounter(&start_);
    }

    ~ScopedTraceDuration() {
        if (!enabled_) return;

        LARGE_INTEGER end{};
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceCounter(&end) || !QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0) {
            return;
        }
        const double milliseconds =
            (static_cast<double>(end.QuadPart - start_.QuadPart) * 1000.0) /
            static_cast<double>(frequency.QuadPart);
        if (milliseconds < 5.0) return;

        wchar_t line[256]{};
        swprintf_s(line, L"%s %.2fms", label_, milliseconds);
        Trace(line);
    }

private:
    const wchar_t* label_;
    bool enabled_{false};
    LARGE_INTEGER start_{};
};

}  // namespace tekito::tsf
