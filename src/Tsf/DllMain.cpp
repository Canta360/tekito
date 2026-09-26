#include "Tsf/ClassFactory.h"
#include "Tsf/Diagnostics.h"
#include "Tsf/Globals.h"
#include "Tsf/Registration.h"
#include "Tsf/TekitoGuids.h"

#include <windows.h>
#include <atomic>
#include <new>

HINSTANCE g_moduleInstance = nullptr;
std::atomic<long> g_objectCount{0};
std::atomic<long> g_serverLockCount{0};

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_moduleInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return g_objectCount.load() == 0 && g_serverLockCount.load() == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    wchar_t hostPath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, hostPath, MAX_PATH) > 0) {
        wchar_t line[MAX_PATH + 32]{};
        swprintf_s(line, L"Host process: %s", hostPath);
        tekito::tsf::Trace(line);
    }
    tekito::tsf::Trace(L"DllGetClassObject");
    if (clsid != CLSID_TekitoTextService) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) tekito::tsf::ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, object);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllRegisterServer() { return RegisterTekitoServer(); }
extern "C" HRESULT __stdcall DllUnregisterServer() { return UnregisterTekitoServer(); }
