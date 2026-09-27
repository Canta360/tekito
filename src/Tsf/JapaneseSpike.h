#pragma once

// Phase 0 probe for Japanese input, built only with -DTEKITO_JA_SPIKE=ON.
// It registers a ja-JP profile and writes to the trace log which of the
// keys a Japanese IME needs reach TEKITO, and how the open/close and
// conversion-mode compartments change. It never logs a key that types a
// character: only the IME keys in the allowlist, and Alt+` / Ctrl+Space.

#if defined(TEKITO_JA_SPIKE)

#include <msctf.h>
#include <windows.h>

namespace tekito::tsf::spike {

void TraceKey(const wchar_t* stage, WPARAM wParam, LPARAM lParam) noexcept;
void TraceProfile(const wchar_t* stage) noexcept;
void TraceCompartment(ITfThreadMgr* threadManager, REFGUID compartment) noexcept;

// Watches the conversion-mode compartment and profile switches for the
// thread; call Stop before the thread manager goes away.
class Watcher final {
public:
    void Start(ITfThreadMgr* threadManager) noexcept;
    void Stop() noexcept;

private:
    ITfThreadMgr* threadManager_{nullptr};
    IUnknown* sink_{nullptr};
    DWORD conversionCookie_{TF_INVALID_COOKIE};
    DWORD profileCookie_{TF_INVALID_COOKIE};
};

}  // namespace tekito::tsf::spike

#endif
