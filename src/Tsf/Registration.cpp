#include "Tsf/Registration.h"

#include "Tsf/Globals.h"
#include "Tsf/TekitoGuids.h"
#include "Tsf/ComPtr.h"

#include <msctf.h>
#include <cwchar>
#include <iterator>
#include <string>

namespace {

std::wstring GuidString(REFGUID guid) {
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

HRESULT SetRegistryString(HKEY root, const std::wstring& path,
                          const wchar_t* valueName, const std::wstring& value) {
    HKEY key{};
    const LONG created = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0,
                                         KEY_WRITE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) return HRESULT_FROM_WIN32(created);
    const LONG result = RegSetValueExW(key, valueName, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(value.c_str()),
                                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(result);
}

HRESULT RegisterComServer() {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_moduleInstance, modulePath, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const std::wstring clsid = GuidString(CLSID_TekitoTextService);
    const std::wstring base = L"CLSID\\" + clsid;
    HRESULT hr = SetRegistryString(HKEY_CLASSES_ROOT, base, nullptr, L"TEKITO English Text Service");
    if (FAILED(hr)) return hr;
    hr = SetRegistryString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32", nullptr, modulePath);
    if (FAILED(hr)) return hr;
    return SetRegistryString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32",
                             L"ThreadingModel", L"Apartment");
}

#if defined(TEKITO_JA_SPIKE)
// The probe takes a substitute keyboard layout for each profile from the
// environment of the registering process, e.g. TEKITO_SPIKE_JA_HKL=04090409.
HKL SpikeSubstituteLayout(const wchar_t* variable) {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(variable, value, static_cast<DWORD>(std::size(value)));
    if (length == 0 || length >= std::size(value)) return nullptr;
    return reinterpret_cast<HKL>(static_cast<ULONG_PTR>(wcstoul(value, nullptr, 16)));
}

HRESULT RegisterJapaneseSpikeProfile(ITfInputProcessorProfileMgr* profiles,
                                     ITfInputProcessorProfiles* legacyProfiles,
                                     const wchar_t* modulePath) {
    constexpr LANGID langid = MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN);
    constexpr wchar_t description[] = L"TEKITO";
    HRESULT hr = profiles->RegisterProfile(CLSID_TekitoTextService, langid, GUID_TekitoJapaneseProfile,
                                           description, static_cast<ULONG>(std::size(description) - 1),
                                           modulePath, static_cast<ULONG>(wcslen(modulePath)), 0,
                                           SpikeSubstituteLayout(L"TEKITO_SPIKE_JA_HKL"), 0, TRUE, 0);
    if (FAILED(hr)) return hr;
    return legacyProfiles->EnableLanguageProfile(CLSID_TekitoTextService, langid,
                                                 GUID_TekitoJapaneseProfile, TRUE);
}
#endif

HRESULT RegisterTsfProfile() {
    constexpr LANGID langid = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    constexpr wchar_t description[] = L"TEKITO English";
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_moduleInstance, modulePath, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    tekito::tsf::ComPtr<ITfInputProcessorProfileMgr> profiles;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(profiles.Put()));
    if (FAILED(hr)) return hr;

    hr = profiles->RegisterProfile(CLSID_TekitoTextService,
                                   langid,
                                   GUID_TekitoEnglishProfile,
                                   description,
                                   static_cast<ULONG>(std::size(description) - 1),
                                   modulePath,
                                   static_cast<ULONG>(wcslen(modulePath)),
                                   0,
#if defined(TEKITO_JA_SPIKE)
                                   SpikeSubstituteLayout(L"TEKITO_SPIKE_EN_HKL"),
#else
                                   nullptr,
#endif
                                   0,
                                   TRUE,
                                   0);
    if (FAILED(hr)) return hr;

    tekito::tsf::ComPtr<ITfInputProcessorProfiles> legacyProfiles;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(legacyProfiles.Put()));
    if (FAILED(hr)) return hr;

    hr = legacyProfiles->EnableLanguageProfile(CLSID_TekitoTextService,
                                               langid,
                                               GUID_TekitoEnglishProfile,
                                               TRUE);
    if (FAILED(hr)) return hr;
#if defined(TEKITO_JA_SPIKE)
    hr = RegisterJapaneseSpikeProfile(profiles.Get(), legacyProfiles.Get(), modulePath);
    if (FAILED(hr)) return hr;
#endif

    tekito::tsf::ComPtr<ITfCategoryMgr> categories;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(categories.Put()));
    if (FAILED(hr)) return hr;

    hr = categories->RegisterCategory(CLSID_TekitoTextService,
                                      GUID_TFCAT_TIP_KEYBOARD,
                                      CLSID_TekitoTextService);
    if (FAILED(hr)) return hr;

    hr = categories->RegisterCategory(CLSID_TekitoTextService,
                                      GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
                                      CLSID_TekitoTextService);
    if (FAILED(hr)) return hr;

    return categories->RegisterCategory(CLSID_TekitoTextService,
                                        GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                                        CLSID_TekitoTextService);
}

HRESULT UnregisterTsfProfile() {
    constexpr LANGID langid = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    HRESULT firstFailure = S_OK;
    const auto rememberFailure = [&](HRESULT hr) {
        if (FAILED(hr) && SUCCEEDED(firstFailure)) firstFailure = hr;
    };

    tekito::tsf::ComPtr<ITfCategoryMgr> categories;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(categories.Put()));
    if (SUCCEEDED(hr)) {
        rememberFailure(categories->UnregisterCategory(CLSID_TekitoTextService,
                                                       GUID_TFCAT_TIP_KEYBOARD,
                                                       CLSID_TekitoTextService));
        rememberFailure(categories->UnregisterCategory(CLSID_TekitoTextService,
                                                       GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
                                                       CLSID_TekitoTextService));
        rememberFailure(categories->UnregisterCategory(CLSID_TekitoTextService,
                                                       GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
                                                       CLSID_TekitoTextService));
    } else {
        rememberFailure(hr);
    }

    tekito::tsf::ComPtr<ITfInputProcessorProfileMgr> profileManager;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(profileManager.Put()));
    if (SUCCEEDED(hr)) {
        rememberFailure(profileManager->UnregisterProfile(CLSID_TekitoTextService,
                                                          langid,
                                                          GUID_TekitoEnglishProfile,
                                                          0));
#if defined(TEKITO_JA_SPIKE)
        rememberFailure(profileManager->UnregisterProfile(
            CLSID_TekitoTextService, MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN),
            GUID_TekitoJapaneseProfile, 0));
#endif
    } else {
        rememberFailure(hr);
    }

    tekito::tsf::ComPtr<ITfInputProcessorProfiles> profiles;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(profiles.Put()));
    if (SUCCEEDED(hr)) {
        rememberFailure(profiles->RemoveLanguageProfile(CLSID_TekitoTextService,
                                                        langid,
                                                        GUID_TekitoEnglishProfile));
        rememberFailure(profiles->Unregister(CLSID_TekitoTextService));
    } else {
        rememberFailure(hr);
    }
    return firstFailure;
}

}  // namespace

HRESULT RegisterTekitoServer() {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(hrInit);

    HRESULT hr = RegisterComServer();
    if (SUCCEEDED(hr)) hr = RegisterTsfProfile();

    if (uninitialize) CoUninitialize();
    return hr;
}

HRESULT UnregisterTekitoServer() {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(hrInit);

    const HRESULT profileResult = UnregisterTsfProfile();
    const std::wstring path = L"CLSID\\" + GuidString(CLSID_TekitoTextService);
    const LONG result = RegDeleteTreeW(HKEY_CLASSES_ROOT, path.c_str());

    if (uninitialize) CoUninitialize();
    if (FAILED(profileResult)) return profileResult;
    return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND
               ? S_OK
               : HRESULT_FROM_WIN32(result);
}
