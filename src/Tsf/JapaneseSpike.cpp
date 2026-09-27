#include "Tsf/JapaneseSpike.h"

#if defined(TEKITO_JA_SPIKE)

#include "Tsf/ComPtr.h"
#include "Tsf/Diagnostics.h"

#include <atomic>
#include <cwchar>
#include <new>

namespace tekito::tsf::spike {
namespace {

bool Down(int key) { return (GetKeyState(key) & 0x8000) != 0; }

// Keys that never type a character on their own. Anything else is left out
// of the log, so the log cannot spell what the user typed.
const wchar_t* ImeKeyName(WPARAM key) {
    switch (key) {
    case 0x14: return L"CapsLock";
    case 0x15: return L"Kana";
    case 0x16: return L"ImeOn";
    case 0x19: return L"Kanji";
    case 0x1A: return L"ImeOff";
    case 0x1C: return L"Convert";
    case 0x1D: return L"NonConvert";
    case 0xF0: return L"DbeAlphanumeric";
    case 0xF1: return L"DbeKatakana";
    case 0xF2: return L"DbeHiragana";
    case 0xF3: return L"DbeSbcs(HankakuZenkaku)";
    case 0xF4: return L"DbeDbcs(HankakuZenkaku)";
    case 0xF5: return L"DbeRoman";
    case 0xF6: return L"DbeNoRoman";
    case VK_F6: return L"F6";
    case VK_F7: return L"F7";
    case VK_F8: return L"F8";
    case VK_F9: return L"F9";
    case VK_F10: return L"F10";
    default: break;
    }
    if (key == VK_OEM_3 && Down(VK_MENU)) return L"Alt+Backquote";
    if (key == VK_SPACE && Down(VK_CONTROL)) return L"Ctrl+Space";
    return nullptr;
}

void ActiveProfile(LANGID& langid, GUID& profile) {
    langid = 0;
    profile = GUID_NULL;
    ComPtr<ITfInputProcessorProfileMgr> manager;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(manager.Put())))) {
        return;
    }
    TF_INPUTPROCESSORPROFILE active{};
    if (SUCCEEDED(manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active))) {
        langid = active.langid;
        profile = active.guidProfile;
    }
}

class ProfileSink final : public ITfInputProcessorProfileActivationSink,
                          public ITfCompartmentEventSink {
public:
    explicit ProfileSink(ITfThreadMgr* threadManager) : threadManager_(threadManager) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_INVALIDARG;
        if (riid == IID_IUnknown || riid == IID_ITfInputProcessorProfileActivationSink) {
            *object = static_cast<ITfInputProcessorProfileActivationSink*>(this);
        } else if (riid == IID_ITfCompartmentEventSink) {
            *object = static_cast<ITfCompartmentEventSink*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --refs_;
        if (value == 0) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE OnActivated(DWORD profileType, LANGID langid, REFCLSID, REFGUID,
                                          REFGUID, HKL hkl, DWORD flags) override {
        wchar_t line[160]{};
        swprintf_s(line, L"Spike ProfileActivated type=%lu langid=0x%04X hkl=%p flags=0x%lX", profileType,
                   langid, static_cast<void*>(hkl), flags);
        Trace(line);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnChange(REFGUID compartment) override {
        TraceCompartment(threadManager_, compartment);
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    ITfThreadMgr* threadManager_;
};

ComPtr<ITfCompartment> Compartment(ITfThreadMgr* threadManager, REFGUID guid) {
    ComPtr<ITfCompartmentMgr> manager;
    ComPtr<ITfCompartment> compartment;
    if (threadManager &&
        SUCCEEDED(threadManager->QueryInterface(IID_ITfCompartmentMgr, reinterpret_cast<void**>(manager.Put())))) {
        (void)manager->GetCompartment(guid, compartment.Put());
    }
    return compartment;
}

DWORD Advise(IUnknown* source, REFIID iid, IUnknown* sink) {
    ComPtr<ITfSource> tfSource;
    DWORD cookie = TF_INVALID_COOKIE;
    if (source && SUCCEEDED(source->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(tfSource.Put())))) {
        (void)tfSource->AdviseSink(iid, sink, &cookie);
    }
    return cookie;
}

void Unadvise(IUnknown* source, DWORD& cookie) {
    if (cookie == TF_INVALID_COOKIE) return;
    ComPtr<ITfSource> tfSource;
    if (source && SUCCEEDED(source->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(tfSource.Put())))) {
        (void)tfSource->UnadviseSink(cookie);
    }
    cookie = TF_INVALID_COOKIE;
}

}  // namespace

void TraceKey(const wchar_t* stage, WPARAM wParam, LPARAM lParam) noexcept {
    const wchar_t* name = ImeKeyName(wParam);
    if (!name) return;
    LANGID langid{};
    GUID profile{};
    ActiveProfile(langid, profile);
    wchar_t line[200]{};
    swprintf_s(line, L"Spike %s key=%s vk=0x%02X scan=0x%02X shift=%d ctrl=%d alt=%d langid=0x%04X hkl=%p", stage,
               name, static_cast<unsigned>(wParam), static_cast<unsigned>((lParam >> 16) & 0xFF),
               Down(VK_SHIFT), Down(VK_CONTROL), Down(VK_MENU), langid,
               static_cast<void*>(GetKeyboardLayout(0)));
    Trace(line);
}

void TraceProfile(const wchar_t* stage) noexcept {
    LANGID langid{};
    GUID profile{};
    ActiveProfile(langid, profile);
    wchar_t guid[64]{};
    StringFromGUID2(profile, guid, 64);
    wchar_t line[200]{};
    swprintf_s(line, L"Spike %s langid=0x%04X profile=%s hkl=%p", stage, langid, guid,
               static_cast<void*>(GetKeyboardLayout(0)));
    Trace(line);
}

void TraceCompartment(ITfThreadMgr* threadManager, REFGUID guid) noexcept {
    const wchar_t* name = guid == GUID_COMPARTMENT_KEYBOARD_OPENCLOSE ? L"OpenClose"
                          : guid == GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION ? L"Conversion"
                                                                                     : nullptr;
    if (!name) return;
    auto compartment = Compartment(threadManager, guid);
    VARIANT value;
    VariantInit(&value);
    const bool read = compartment && SUCCEEDED(compartment->GetValue(&value)) && value.vt == VT_I4;
    wchar_t line[120]{};
    swprintf_s(line, L"Spike Compartment %s=%s0x%lX", name, read ? L"" : L"(unset)",
               read ? static_cast<unsigned long>(value.lVal) : 0ul);
    VariantClear(&value);
    Trace(line);
}

void Watcher::Start(ITfThreadMgr* threadManager) noexcept {
    Stop();
    if (!threadManager) return;
    auto* sink = new (std::nothrow) ProfileSink(threadManager);
    if (!sink) return;
    threadManager_ = threadManager;
    threadManager_->AddRef();
    sink_ = static_cast<ITfInputProcessorProfileActivationSink*>(sink);
    profileCookie_ = Advise(threadManager_, IID_ITfInputProcessorProfileActivationSink, sink_);
    auto conversion = Compartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    conversionCookie_ = Advise(conversion.Get(), IID_ITfCompartmentEventSink, sink_);
    TraceProfile(L"WatcherStart");
    TraceCompartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    TraceCompartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
}

void Watcher::Stop() noexcept {
    if (!threadManager_) return;
    Unadvise(threadManager_, profileCookie_);
    auto conversion = Compartment(threadManager_, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
    Unadvise(conversion.Get(), conversionCookie_);
    if (sink_) sink_->Release();
    sink_ = nullptr;
    threadManager_->Release();
    threadManager_ = nullptr;
}

}  // namespace tekito::tsf::spike

#endif
