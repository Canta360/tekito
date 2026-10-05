#include "Tsf/Registration.h"

#include "Tsf/Globals.h"
#include "Tsf/TekitoGuids.h"
#include "Tsf/ComPtr.h"
#include "UserData/KeyboardLayout.h"
#include "UserData/UserDataRepository.h"
#include "UserData/UserSettings.h"

#include <msctf.h>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

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
    HRESULT hr = SetRegistryString(HKEY_CLASSES_ROOT, base, nullptr, L"TEKITO Text Service");
    if (FAILED(hr)) return hr;
    hr = SetRegistryString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32", nullptr, modulePath);
    if (FAILED(hr)) return hr;
    return SetRegistryString(HKEY_CLASSES_ROOT, base + L"\\InprocServer32",
                             L"ThreadingModel", L"Apartment");
}

struct Profile {
    LANGID langid;
    const GUID* guid;
    const wchar_t* description;
    HKL layout;
};

// The English profile, and the Japanese one, which also carries TEKITO's
// English modes. On a Japanese keyboard the English profile types with the
// Japanese layout so symbols match the keys (see KeyboardLayout.h).
std::vector<Profile> Profiles() {
    tekito::userdata::UserSettings settings;
    if (auto repository = tekito::userdata::CreateDefaultUserDataRepository();
        repository && repository->Open()) {
        (void)repository->LoadSettings(settings);
        repository->Close();
    }
    return {
        {MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), &GUID_TekitoEnglishProfile, L"TEKITO English",
         tekito::userdata::EnglishProfileLayout(settings.keyboardType)},
        {MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN), &GUID_TekitoJapaneseProfile, L"TEKITO",
         nullptr},
    };
}

const GUID* const kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    // Windows offers TEKITO in the Start menu's search, Settings and Store
    // apps too (they run in an app container: the installer lets them read
    // the program and its Data Packs).
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    // Windows shows the Japanese profile's mode from the conversion-mode
    // compartment.
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
};

HRESULT RegisterTsfProfile() {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_moduleInstance, modulePath, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    tekito::tsf::ComPtr<ITfInputProcessorProfileMgr> profiles;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(profiles.Put()));
    if (FAILED(hr)) return hr;
    tekito::tsf::ComPtr<ITfInputProcessorProfiles> legacyProfiles;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(legacyProfiles.Put()));
    if (FAILED(hr)) return hr;

    for (const auto& profile : Profiles()) {
        hr = profiles->RegisterProfile(CLSID_TekitoTextService, profile.langid, *profile.guid,
                                       profile.description,
                                       static_cast<ULONG>(wcslen(profile.description)), modulePath,
                                       static_cast<ULONG>(wcslen(modulePath)), 0, profile.layout, 0,
                                       TRUE, 0);
        if (FAILED(hr)) return hr;
        hr = legacyProfiles->EnableLanguageProfile(CLSID_TekitoTextService, profile.langid,
                                                   *profile.guid, TRUE);
        if (FAILED(hr)) return hr;
    }

    tekito::tsf::ComPtr<ITfCategoryMgr> categories;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(categories.Put()));
    if (FAILED(hr)) return hr;
    for (const GUID* category : kCategories) {
        hr = categories->RegisterCategory(CLSID_TekitoTextService, *category, CLSID_TekitoTextService);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT UnregisterTsfProfile() {
    HRESULT firstFailure = S_OK;
    const auto rememberFailure = [&](HRESULT hr) {
        if (FAILED(hr) && SUCCEEDED(firstFailure)) firstFailure = hr;
    };

    tekito::tsf::ComPtr<ITfCategoryMgr> categories;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(categories.Put()));
    if (SUCCEEDED(hr)) {
        for (const GUID* category : kCategories) {
            // A category an older version never registered is not an error.
            (void)categories->UnregisterCategory(CLSID_TekitoTextService, *category,
                                                 CLSID_TekitoTextService);
        }
    } else {
        rememberFailure(hr);
    }

    tekito::tsf::ComPtr<ITfInputProcessorProfileMgr> profileManager;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(profileManager.Put()));
    if (SUCCEEDED(hr)) {
        for (const auto& profile : Profiles()) {
            // Likewise a profile an older version never registered.
            (void)profileManager->UnregisterProfile(CLSID_TekitoTextService, profile.langid,
                                                    *profile.guid, 0);
        }
    } else {
        rememberFailure(hr);
    }

    tekito::tsf::ComPtr<ITfInputProcessorProfiles> profiles;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(profiles.Put()));
    if (SUCCEEDED(hr)) {
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
