#include "Tsf/TextService.h"

#include "Tsf/ComPtr.h"
#include "Tsf/Compartments.h"
#include "Tsf/DisplayAttributes.h"
#include "Tsf/Diagnostics.h"
#include "Tsf/Globals.h"
#include "Tsf/TekitoGuids.h"
#include "UserData/KeyboardLayout.h"
#include "UserData/UiLanguage.h"

#include <inputscope.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <shellapi.h>
#include <filesystem>
#include <iterator>
#include <new>
#include <oleauto.h>
#include <vector>

namespace tekito::tsf {
namespace {

constexpr GUID kGuidPropInputScope = {0x1713dd5a,
                                      0x68e7,
                                      0x4a5b,
                                      {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

bool HasBlockedModifier() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetKeyState(VK_MENU) & 0x8000) != 0 ||
           (GetKeyState(VK_LWIN) & 0x8000) != 0 ||
           (GetKeyState(VK_RWIN) & 0x8000) != 0;
}

bool TryClassifyPunctuation(wchar_t ch, PunctuationRole& role) {
    switch (ch) {
    case L'.':
    case L'!':
    case L'?':
        role = PunctuationRole::SentenceTerminal;
        return true;
    case L',':
    case L':':
    case L';':
        role = PunctuationRole::ClauseSeparator;
        return true;
    default:
        return false;
    }
}

// Fields where correcting words would be wrong or unsafe: secrets, and
// fields that hold addresses, numbers, paths or code rather than prose.
bool IsPassThroughInputScope(InputScope scope) {
    switch (scope) {
    case IS_PASSWORD:
    case IS_PRIVATE:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
    case IS_URL:
    case IS_EMAIL_USERNAME:
    case IS_EMAIL_SMTPEMAILADDRESS:
    case IS_LOGINNAME:
    case IS_DIGITS:
    case IS_NUMBER:
    case IS_NUMBER_FULLWIDTH:
    case IS_TELEPHONE_FULLTELEPHONENUMBER:
    case IS_TELEPHONE_COUNTRYCODE:
    case IS_TELEPHONE_AREACODE:
    case IS_TELEPHONE_LOCALNUMBER:
    case IS_CURRENCY_AMOUNTANDSYMBOL:
    case IS_CURRENCY_AMOUNT:
    case IS_DATE_FULLDATE:
    case IS_DATE_MONTH:
    case IS_DATE_DAY:
    case IS_DATE_YEAR:
    case IS_TIME_FULLTIME:
    case IS_TIME_HOUR:
    case IS_TIME_MINORSEC:
    case IS_FILE_FULLFILEPATH:
    case IS_FILE_FILENAME:
    case IS_REGULAREXPRESSION:
    case IS_SRGS:
    case IS_XML:
        return true;
    default:
        return false;
    }
}

// Letters and the apostrophe inside contractions ("don't", "it’s").
bool IsWordCharacter(wchar_t ch) {
    return std::iswalpha(ch) != 0 || ch == L'\'' || ch == L'\u2019';
}

// A word may begin after whitespace, an opening bracket or quote, a dash, or
// Japanese text.
// Right after anything else -- a letter or digit we did not compose, or the
// '/', '.', '@', ':', '_' of a URL, address, path or identifier -- the typing
// is part of a token that must be left exactly as typed.
bool CanStartWordAfter(std::wstring_view preceding) {
    if (preceding.empty()) return true;
    const wchar_t ch = preceding.back();
    if (std::iswspace(ch)) return true;
    // Japanese text puts English words right after kana, kanji or
    // full-width marks, with no space ("Macを使う", "これはMac").
    if (ch >= L'　') return true;
    switch (ch) {
    case L'(': case L'[': case L'{': case L'<':
    case L'"': case L'\'': case L'\u201C': case L'\u2018': case L'\u00AB':
    case L'-': case L'\u2013': case L'\u2014':
    case L'*':
        return true;
    default:
        return false;
    }
}

std::wstring ReadRangeText(ITfRange* range, TfEditCookie editCookie) {
    if (!range) return {};
    wchar_t buffer[256]{};
    ULONG length = 0;
    if (FAILED(range->GetText(editCookie, 0, buffer, 256, &length))) return {};
    return std::wstring(buffer, std::min<ULONG>(length, 255));
}

void ReadCompositionContext(ITfComposition* composition, TfEditCookie editCookie,
                            std::wstring& preceding, std::wstring& following) {
    if (!composition) return;
    ComPtr<ITfRange> compositionRange;
    if (FAILED(composition->GetRange(compositionRange.Put()))) return;

    ComPtr<ITfRange> before;
    if (SUCCEEDED(compositionRange->Clone(before.Put())) &&
        SUCCEEDED(before->Collapse(editCookie, TF_ANCHOR_START))) {
        LONG moved = 0;
        if (SUCCEEDED(before->ShiftStart(editCookie, -128, &moved, nullptr))) {
            preceding = ReadRangeText(before.Get(), editCookie);
        }
    }

    ComPtr<ITfRange> after;
    if (SUCCEEDED(compositionRange->Clone(after.Put())) &&
        SUCCEEDED(after->Collapse(editCookie, TF_ANCHOR_END))) {
        LONG moved = 0;
        if (SUCCEEDED(after->ShiftEnd(editCookie, 128, &moved, nullptr))) {
            following = ReadRangeText(after.Get(), editCookie);
        }
    }
}

std::wstring ReadSelectionContext(ITfContext* context, TfEditCookie editCookie) {
    if (!context) return {};
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched)) || fetched != 1 || !selection.range) {
        return {};
    }
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    if (FAILED(range->Collapse(editCookie, TF_ANCHOR_START))) return {};
    LONG moved = 0;
    if (FAILED(range->ShiftStart(editCookie, -256, &moved, nullptr))) return {};
    return ReadRangeText(range.Get(), editCookie);
}

HRESULT ReplaceTrailingWhitespaceAtSelection(ITfContext* context, TfEditCookie editCookie,
                                             std::size_t whitespaceCount,
                                             const std::wstring& text) {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched)) || fetched != 1 || !selection.range) {
        return E_FAIL;
    }
    ComPtr<ITfRange> range;
    range.Attach(selection.range);
    HRESULT hr = range->Collapse(editCookie, TF_ANCHOR_START);
    if (FAILED(hr)) return hr;
    LONG moved = 0;
    hr = range->ShiftStart(editCookie, -static_cast<LONG>(whitespaceCount), &moved, nullptr);
    if (FAILED(hr) || moved != -static_cast<LONG>(whitespaceCount)) return E_FAIL;
    hr = range->SetText(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (FAILED(hr)) return hr;
    hr = range->Collapse(editCookie, TF_ANCHOR_END);
    if (FAILED(hr)) return hr;
    TF_SELECTION next{};
    next.range = range.Get();
    next.style.ase = TF_AE_END;
    next.style.fInterimChar = FALSE;
    return context->SetSelection(editCookie, 1, &next);
}

bool ContextHasPassThroughInputScope(ITfContext* context, TfEditCookie editCookie) {
    if (!context) return false;

    ComPtr<ITfReadOnlyProperty> property;
    if (FAILED(context->GetAppProperty(kGuidPropInputScope, property.Put()))) return false;

    TF_SELECTION selection{};
    ULONG fetched = 0;
    ComPtr<ITfRange> range;
    if (SUCCEEDED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) &&
        fetched == 1 && selection.range) {
        range.Attach(selection.range);
    }

    VARIANT value{};
    VariantInit(&value);
    HRESULT hr = property->GetValue(editCookie, range.Get(), &value);
    if (FAILED(hr) || value.vt != VT_UNKNOWN || !value.punkVal) {
        VariantClear(&value);
        return false;
    }

    ComPtr<ITfInputScope> inputScope;
    hr = value.punkVal->QueryInterface(IID_PPV_ARGS(inputScope.Put()));
    VariantClear(&value);
    if (FAILED(hr)) return false;

    InputScope* scopes = nullptr;
    UINT count = 0;
    hr = inputScope->GetInputScopes(&scopes, &count);
    if (FAILED(hr)) return false;

    bool passThrough = false;
    for (UINT i = 0; i < count; ++i) {
        if (IsPassThroughInputScope(scopes[i])) {
            passThrough = true;
            break;
        }
    }
    CoTaskMemFree(scopes);
    return passThrough;
}

// No document, or one the application marks read-only: keys are not ours.
bool IsReadOnly(ITfContext* context) {
    if (!context) return true;
    TF_STATUS status{};
    return SUCCEEDED(context->GetStatus(&status)) && (status.dwDynamicFlags & TF_SD_READONLY) != 0;
}

bool FocusIsClassicPasswordEdit() {
    HWND focus = GetFocus();
    if (!focus) return false;

    wchar_t className[32]{};
    if (GetClassNameW(focus, className, static_cast<int>(std::size(className))) <= 0) {
        return false;
    }
    if (std::wcscmp(className, L"Edit") != 0) return false;

    const LONG_PTR style = GetWindowLongPtrW(focus, GWL_STYLE);
    if ((style & ES_PASSWORD) != 0) return true;

    return SendMessageW(focus, EM_GETPASSWORDCHAR, 0, 0) != 0;
}

bool CurrentProcessIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring CurrentProcessName() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    return std::filesystem::path(path, path + length).filename().wstring();
}

bool IsBuiltInPassThroughProcess(const std::wstring& name) {
    const auto& apps = userdata::kBuiltInExcludedApps;
    return std::any_of(std::begin(apps), std::end(apps), [&](const wchar_t* app) {
        return _wcsicmp(name.c_str(), app) == 0;
    });
}

bool ToggleKeyFor(int key, TF_PRESERVEDKEY& preserved) {
    switch (key) {
    case 1:
        // A US keyboard's stand-in for Hankaku/Zenkaku.
        preserved = {VK_OEM_3, TF_MOD_ALT};
        return true;
    case 2:
        preserved = {VK_SPACE, TF_MOD_CONTROL};
        return true;
    case 3:
        preserved = {VK_SPACE, TF_MOD_CONTROL | TF_MOD_SHIFT};
        return true;
    default:
        return false;
    }
}

ComPtr<ITfCompartment> OpenCloseCompartment(ITfThreadMgr* threadManager) {
    return ThreadCompartment(threadManager, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
}

ComPtr<ITfCompartment> ConversionCompartment(ITfThreadMgr* threadManager) {
    return ThreadCompartment(threadManager, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
}

HRESULT AdviseSink(IUnknown* source, REFIID riid, IUnknown* sink, DWORD& cookie) {
    ComPtr<ITfSource> tfSource;
    HRESULT hr = source->QueryInterface(IID_PPV_ARGS(tfSource.Put()));
    if (SUCCEEDED(hr)) hr = tfSource->AdviseSink(riid, sink, &cookie);
    if (FAILED(hr)) cookie = TF_INVALID_COOKIE;
    return hr;
}

void UnadviseSink(IUnknown* source, DWORD& cookie) {
    if (cookie == TF_INVALID_COOKIE) return;
    ComPtr<ITfSource> tfSource;
    if (source && SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(tfSource.Put())))) {
        tfSource->UnadviseSink(cookie);
    }
    cookie = TF_INVALID_COOKIE;
}

DWORD WINAPI PreloadEngineDataThread(void* parameter) {
    const auto module = static_cast<HMODULE>(parameter);
    try {
        PreloadDefaultEngineData();
    } catch (...) {
        Trace(L"Engine data preload failed");
    }
    // The thread holds its own reference on this DLL so it can finish even
    // if the text service is released meanwhile.
    FreeLibraryAndExitThread(module, 0);
}

// Loads the language data once per process, on a worker thread.
void StartEngineDataPreload() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&PreloadEngineDataThread), &module)) {
        started = false;
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, &PreloadEngineDataThread, module, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        FreeLibrary(module);
        started = false;
    }
}

bool IsNavigationKey(WPARAM key) {
    switch (key) {
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_DELETE:
    case VK_INSERT:
        return true;
    default:
        return false;
    }
}

class DisplayAttributeInfoEnum final : public IEnumTfDisplayAttributeInfo {
public:
    // Takes a reference to each info.
    explicit DisplayAttributeInfoEnum(std::vector<ITfDisplayAttributeInfo*> infos, std::size_t next = 0)
        : infos_(std::move(infos)), next_(next) {
        for (auto* info : infos_) {
            if (info) info->AddRef();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) {
            *object = static_cast<IEnumTfDisplayAttributeInfo*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG value = --refCount_;
        if (value == 0) delete this;
        return value;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo** clone) override {
        if (!clone) return E_INVALIDARG;
        *clone = new (std::nothrow) DisplayAttributeInfoEnum(infos_, next_);
        return *clone ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo** info, ULONG* fetched) override {
        if (!info || (count != 1 && !fetched)) return E_INVALIDARG;
        ULONG taken = 0;
        while (taken < count && next_ < infos_.size()) {
            info[taken] = infos_[next_++];
            info[taken]->AddRef();
            ++taken;
        }
        if (fetched) *fetched = taken;
        return taken == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Reset() override {
        next_ = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const std::size_t left = infos_.size() - next_;
        next_ += std::min<std::size_t>(count, left);
        return count <= left ? S_OK : S_FALSE;
    }

private:
    ~DisplayAttributeInfoEnum() {
        for (auto* info : infos_) {
            if (info) info->Release();
        }
    }

    std::atomic<ULONG> refCount_{1};
    std::vector<ITfDisplayAttributeInfo*> infos_;
    std::size_t next_{0};
};

}  // namespace

TextService::TextService()
    : userLearningProvider_(userLearning_),
      socialLearningProvider_(socialLearning_),
      settingsRepository_(userdata::CreateDefaultUserDataRepository()),
      processIsElevated_(CurrentProcessIsElevated()),
      processName_(CurrentProcessName()) {
    processPrefersPassThrough_ = IsBuiltInPassThroughProcess(processName_);
    ++g_objectCount;
    Trace(L"TextService ctor");
}

TextService::~TextService() {
    Trace(L"TextService dtor");
    Deactivate();
    --g_objectCount;
}

HRESULT TextService::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;

    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor) {
        *object = static_cast<ITfTextInputProcessor*>(this);
    } else if (riid == IID_ITfTextInputProcessorEx) {
        *object = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (riid == IID_ITfKeyEventSink) {
        *object = static_cast<ITfKeyEventSink*>(this);
    } else if (riid == IID_ITfCompositionSink) {
        *object = static_cast<ITfCompositionSink*>(this);
    } else if (riid == IID_ITfDisplayAttributeProvider) {
        *object = static_cast<ITfDisplayAttributeProvider*>(this);
    } else if (riid == IID_ITfDisplayAttributeInfo) {
        *object = static_cast<ITfDisplayAttributeInfo*>(this);
    } else if (riid == IID_ITfThreadMgrEventSink) {
        *object = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (riid == IID_ITfTextEditSink) {
        *object = static_cast<ITfTextEditSink*>(this);
    } else if (riid == IID_ITfTextLayoutSink) {
        *object = static_cast<ITfTextLayoutSink*>(this);
    } else if (riid == IID_ITfCompartmentEventSink) {
        *object = static_cast<ITfCompartmentEventSink*>(this);
    } else if (riid == IID_ITfInputProcessorProfileActivationSink) {
        *object = static_cast<ITfInputProcessorProfileActivationSink*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG TextService::AddRef() { return ++refCount_; }

ULONG TextService::Release() {
    const ULONG value = --refCount_;
    if (value == 0) delete this;
    return value;
}

HRESULT TextService::Activate(ITfThreadMgr* threadManager, TfClientId clientId) {
    return ActivateEx(threadManager, clientId, 0);
}

HRESULT TextService::ActivateEx(ITfThreadMgr* threadManager, TfClientId clientId, DWORD flags) {
    Trace(L"ActivateEx");
    if (!threadManager) return E_INVALIDARG;
    Deactivate();

    threadManager_ = threadManager;
    threadManager_->AddRef();
    clientId_ = clientId;
    activateFlags_ = flags;

    candidateEngine_.reset();
    runtimeMode_.reset();
    userDictionary_ = {};
    userLearning_ = {};
    socialLearning_ = {};
    japaneseLearning_.Clear();
    userSettings_ = {};
    userLearningProvider_.SetEnabled(true);

    if (settingsRepository_ && settingsRepository_->Open()) {
        if (settingsRepository_->Load(userDictionary_)) {
            if (!settingsRepository_->LoadLearning(userLearning_)) {
                Trace(L"ActivateEx UserLearning load skipped");
            }
            if (!settingsRepository_->LoadSocialLearning(socialLearning_)) {
                Trace(L"ActivateEx SocialLearning load skipped");
            }
            if (!settingsRepository_->LoadJapaneseLearning(japaneseLearning_)) {
                Trace(L"ActivateEx JapaneseLearning load skipped");
            }
            if (!settingsRepository_->LoadJapaneseUserWords(japaneseUserWords_)) {
                Trace(L"ActivateEx Japanese user words load skipped");
            }
            Trace(L"ActivateEx UserDictionary loaded");
        } else {
            Trace(L"ActivateEx UserDictionary load skipped");
        }

        if (settingsRepository_->LoadSettings(userSettings_)) {
            settingsLoaded_ = true;
            ApplySettings();
        } else {
            settingsRepository_->Close();
            Trace(L"ActivateEx UserSettings load skipped");
        }
    } else {
        Trace(L"ActivateEx UserData repository open skipped");
    }

    const auto startupMode = userSettings_.restoreLastInputMode
                                 ? userSettings_.lastInputMode
                                 : userSettings_.defaultInputMode;
    const auto japaneseStartupMode = userSettings_.restoreLastInputMode
                                         ? userSettings_.lastJapaneseProfileMode
                                         : InputMode::Japanese;
    runtimeMode_ = std::make_unique<userdata::RuntimeModeState>(
        startupMode, userdata::kRuntimeStateName, japaneseStartupMode);
    UpdateActiveProfile();
    activationSettling_ = japaneseProfile_;
    mode_ = SharedMode();
    state_.SetInputMode(mode_ == InputMode::Convert ? InputMode::Convert : InputMode::Direct);
    runtimeModeGeneration_ = runtimeMode_->ModeGeneration();
    runtimeSettingsGeneration_ = runtimeMode_->SettingsGeneration();
    runtimeDictionaryGeneration_ = runtimeMode_->DictionaryGeneration();
    runtimeLearningGeneration_ = runtimeMode_->LearningGeneration();

    // Loading the language data takes seconds; never do it on the host
    // application's thread. Until it is ready, keys pass through untouched.
    candidateEngine_.reset();
    if (IsDefaultEngineDataLoaded()) {
        EnsureEngine();
    } else {
        StartEngineDataPreload();
    }

    ComPtr<ITfKeystrokeMgr> keystrokeManager;
    HRESULT hr = threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put()));
    if (FAILED(hr)) {
        Trace(L"ActivateEx QueryInterface ITfKeystrokeMgr failed");
        return hr;
    }

    hr = keystrokeManager->AdviseKeyEventSink(clientId_, this, TRUE);
    if (FAILED(hr)) {
        Trace(L"ActivateEx AdviseKeyEventSink failed");
        return hr;
    }

    UpdateToggleKey(userdata::PreservedSwitchKey(userSettings_.toggleKey, userSettings_.keyboardType));
    AdviseThreadManagerSinks();
    SyncModeCompartments();

    RegisterLangBarItem();
    candidateWindow_.Initialize(g_moduleInstance, [this](std::size_t index) {
        OnCandidateSelected(index);
    });
    Trace(L"ActivateEx ready");
    return S_OK;
}

HRESULT TextService::Deactivate() {
    Trace(L"Deactivate");
    if (settingsLoaded_) SyncRuntimeState();
    if (settingsLoaded_) {
        userdata::UserSettings latestSettings = userSettings_;
        if (settingsRepository_) (void)settingsRepository_->LoadSettings(latestSettings);
        latestSettings.lastInputMode = runtimeMode_ ? runtimeMode_->Mode() : state_.Mode();
        if (runtimeMode_) latestSettings.lastJapaneseProfileMode = runtimeMode_->JapaneseMode();
        latestSettings.japaneseProfileEnglishMode = userSettings_.japaneseProfileEnglishMode;
        userSettings_ = latestSettings;
        if (!settingsRepository_ || !settingsRepository_->SaveSettings(latestSettings)) {
            Trace(L"Deactivate UserSettings save failed");
        }
        if (!settingsRepository_->SaveLearning(userLearning_)) {
            Trace(L"Deactivate UserLearning save failed");
        }
        if (!settingsRepository_->SaveSocialLearning(socialLearning_)) {
            Trace(L"Deactivate SocialLearning save failed");
        }
        if (!settingsRepository_->SaveJapaneseLearning(japaneseLearning_)) {
            Trace(L"Deactivate JapaneseLearning save failed");
        }
        if (settingsRepository_) settingsRepository_->Close();
        settingsLoaded_ = false;
    }
    candidateWindow_.Hide();
    state_.Reset();
    rawText_.clear();
    japanese_.Clear();
    UnregisterLangBarItem();
    UnadviseContextSinks();
    UnadviseThreadManagerSinks();
    UpdateToggleKey(0);

    if (threadManager_ && clientId_ != TF_CLIENTID_NULL) {
        ComPtr<ITfKeystrokeMgr> keystrokeManager;
        if (SUCCEEDED(threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put())))) {
            keystrokeManager->UnadviseKeyEventSink(clientId_);
        }
    }

    if (composition_) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (compositionContext_) {
        compositionContext_->Release();
        compositionContext_ = nullptr;
    }
    if (threadManager_) {
        threadManager_->Release();
        threadManager_ = nullptr;
    }
    clientId_ = TF_CLIENTID_NULL;
    activateFlags_ = 0;
    runtimeMode_.reset();
    runtimeModeGeneration_ = 0;
    runtimeSettingsGeneration_ = 0;
    runtimeDictionaryGeneration_ = 0;
    runtimeLearningGeneration_ = 0;
    return S_OK;
}

void TextService::SyncRuntimeState() {
    if (!runtimeMode_) return;
    const auto settingsGeneration = runtimeMode_->SettingsGeneration();
    if (settingsGeneration != runtimeSettingsGeneration_ && settingsLoaded_ &&
        settingsRepository_) {
        userdata::UserSettings loadedSettings;
        if (settingsRepository_->LoadSettings(loadedSettings)) {
            userSettings_ = std::move(loadedSettings);
            ApplySettings();
            // Japanese was turned off in Settings.
            if (mode_ == InputMode::Japanese && !JapaneseModeAvailable()) ChangeInputMode(InputMode::Convert);
        }
        runtimeSettingsGeneration_ = settingsGeneration;
    }

    const auto dictionaryGeneration = runtimeMode_->DictionaryGeneration();
    if (dictionaryGeneration != runtimeDictionaryGeneration_ && settingsLoaded_ &&
        settingsRepository_) {
        UserDictionary loadedDictionary;
        if (settingsRepository_->Load(loadedDictionary)) {
            // The engine reads userDictionary_ by reference; refresh it in
            // place rather than rebuilding (a rebuild reloads every Data
            // Pack and stalls typing for seconds).
            userDictionary_ = std::move(loadedDictionary);
            if (candidateEngine_) candidateEngine_->RefreshUserDictionary();
        }
        std::vector<japanese::UserWord> loadedWords;
        if (settingsRepository_->LoadJapaneseUserWords(loadedWords)) {
            japaneseUserWords_ = std::move(loadedWords);
            if (japaneseProfile_) RefreshJapaneseUserWords();
        }
        runtimeDictionaryGeneration_ = dictionaryGeneration;
    }

    const auto learningGeneration = runtimeMode_->LearningGeneration();
    if (learningGeneration != runtimeLearningGeneration_ && settingsLoaded_ &&
        settingsRepository_) {
        // The learning providers read these stores by reference, so
        // replacing their contents is enough; no engine rebuild.
        UserLearningStore loadedLearning;
        if (settingsRepository_->LoadLearning(loadedLearning)) {
            userLearning_ = std::move(loadedLearning);
        }
        SocialLearningStore loadedSocialLearning;
        if (settingsRepository_->LoadSocialLearning(loadedSocialLearning)) {
            socialLearning_ = std::move(loadedSocialLearning);
        }
        japanese::JapaneseLearningStore loadedJapaneseLearning;
        if (settingsRepository_->LoadJapaneseLearning(loadedJapaneseLearning)) {
            japaneseLearning_ = std::move(loadedJapaneseLearning);
        }
        runtimeLearningGeneration_ = learningGeneration;
    }

    const auto modeGeneration = runtimeMode_->ModeGeneration();
    if (modeGeneration != runtimeModeGeneration_) {
        SetInputMode(SharedMode());
        runtimeModeGeneration_ = modeGeneration;
    }
}

bool TextService::EnsureEngine() {
    if (candidateEngine_) return true;
    if (!IsDefaultEngineDataLoaded()) return false;
    candidateEngine_ = CreateDefaultConversionEngine(userDictionary_, userLearningProvider_,
                                                     socialLearningProvider_);
    if (!candidateEngine_) Trace(L"Engine creation failed");
    return candidateEngine_ != nullptr;
}

void TextService::ApplySettings() {
    userLearningProvider_.SetEnabled(userSettings_.learningEnabled);
    socialLearningProvider_.SetProfile(userSettings_.socialExpressionRange,
                                       userSettings_.socialPersonalization);
    const bool wasPassingThrough = ProcessPassesThrough();
    processExcludedByUser_ = std::any_of(
        userSettings_.excludedApps.begin(), userSettings_.excludedApps.end(),
        [&](const std::wstring& name) { return _wcsicmp(name.c_str(), processName_.c_str()) == 0; });
    if (!wasPassingThrough && ProcessPassesThrough() && state_.IsActive()) {
        FinishCompositionLater();
    }
    if (threadManager_) {
        UpdateToggleKey(userdata::PreservedSwitchKey(userSettings_.toggleKey, userSettings_.keyboardType));
    }
    japanese_.SetPunctuationStyle(
        static_cast<japanese::PunctuationStyle>(std::clamp(userSettings_.japanesePunctuation, 0, 3)));
    japanese_.SetLearning(userSettings_.learningEnabled ? &japaneseLearning_ : nullptr);
    japanese_.SetPredictionEnabled(userSettings_.japanesePredictionEnabled);
    japanese_.SetSpecialConversions(&SpecialConversions::Installed(), SpecialOptions());
    const auto rows = static_cast<std::size_t>(userSettings_.candidateRows);
    state_.SetPageSizes(rows ? rows : 5, rows ? rows : 10);
    japanesePage_ = rows ? rows : 9;
}

// Keys that end the word and still reach the application: anything that is
// not part of a word, and Enter unless it picks a candidate from the list.
bool TextService::LetsKeyThrough(const KeyInput& input) const noexcept {
    // In Japanese, Enter only commits the text, as in Microsoft IME.
    if (mode_ == InputMode::Japanese) return input.type == KeyInput::Type::EndComposition;
    return input.type == KeyInput::Type::EndComposition ||
           (input.type == KeyInput::Type::Enter && !state_.IsCandidateNavigationActive());
}

bool TextService::ProcessPassesThrough() const noexcept {
    return processIsElevated_ || processPrefersPassThrough_ || processExcludedByUser_;
}

void TextService::SetInputMode(InputMode mode) {
    // Only the Japanese profile has a Japanese mode, and only with Japanese on.
    if (!JapaneseModeAvailable() && mode == InputMode::Japanese) mode = InputMode::Convert;
    englishSegment_ = false;
    const bool finishWord = (mode != InputMode::Convert && state_.IsActive()) ||
                            (mode != InputMode::Japanese && japanese_.IsComposing());
    mode_ = mode;
    state_.SetInputMode(mode == InputMode::Convert ? InputMode::Convert : InputMode::Direct);
    if (japaneseProfile_) {
        userSettings_.lastJapaneseProfileMode = mode;
        if (mode != InputMode::Japanese) userSettings_.japaneseProfileEnglishMode = mode;
    } else {
        userSettings_.lastInputMode = mode;
    }
    if (finishWord) FinishCompositionLater();
    SyncModeCompartments();
    if (modeLangBarItem_) modeLangBarItem_->NotifyUpdate();
}

void TextService::ChangeInputMode(InputMode mode) {
    activationSettling_ = false;
    if (runtimeMode_) {
        if (japaneseProfile_) {
            runtimeMode_->SetJapaneseMode(mode);
        } else {
            runtimeMode_->SetMode(mode == InputMode::Direct ? InputMode::Direct : InputMode::Convert);
        }
        runtimeModeGeneration_ = runtimeMode_->ModeGeneration();
    }
    SetInputMode(mode);
}

// Commits the word being typed as it stands, from outside a key press: the
// mode changed, focus moved, or the caret left the word.
void TextService::FinishCompositionLater() {
    candidateWindow_.Hide();
    if (!compositionContext_) {
        state_.Reset();
        rawText_.clear();
        japanese_.Clear();
        return;
    }
    KeyInput input{};
    input.type = japanese_.IsComposing() ? KeyInput::Type::JapaneseCommit
                                         : KeyInput::Type::EndComposition;
    auto* session = new (std::nothrow) KeyEditSession(this, compositionContext_, input);
    HRESULT sessionResult = E_FAIL;
    const HRESULT hr = session ? compositionContext_->RequestEditSession(
                                     clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                     &sessionResult)
                               : E_OUTOFMEMORY;
    if (session) session->Release();
    if (FAILED(hr)) {
        TraceHr(L"FinishCompositionLater RequestEditSession failed", hr);
        state_.Reset();
        rawText_.clear();
        japanese_.Clear();
    }
}

void TextService::UpdateToggleKey(int key) {
    if (ProcessPassesThrough()) key = 0;
    if (key == preservedToggleKey_ || !threadManager_ || clientId_ == TF_CLIENTID_NULL) return;
    ComPtr<ITfKeystrokeMgr> keystrokeManager;
    if (FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(keystrokeManager.Put())))) return;

    TF_PRESERVEDKEY preserved{};
    if (ToggleKeyFor(preservedToggleKey_, preserved)) {
        keystrokeManager->UnpreserveKey(GUID_TekitoToggleKey, &preserved);
    }
    preservedToggleKey_ = 0;
    if (ToggleKeyFor(key, preserved)) {
        constexpr wchar_t kDescription[] = L"Turn TEKITO on or off";
        const HRESULT hr = keystrokeManager->PreserveKey(
            clientId_, GUID_TekitoToggleKey, &preserved, kDescription,
            static_cast<ULONG>(std::size(kDescription) - 1));
        if (SUCCEEDED(hr)) {
            preservedToggleKey_ = key;
        } else {
            TraceHr(L"PreserveKey failed", hr);
        }
    }
}

void TextService::AdviseThreadManagerSinks() {
    if (!threadManager_) return;
    if (threadManagerSinkCookie_ == TF_INVALID_COOKIE) {
        (void)AdviseSink(threadManager_, IID_ITfThreadMgrEventSink,
                         static_cast<ITfThreadMgrEventSink*>(this), threadManagerSinkCookie_);
    }
    if (openCloseSinkCookie_ == TF_INVALID_COOKIE) {
        if (auto compartment = OpenCloseCompartment(threadManager_)) {
            (void)AdviseSink(compartment.Get(), IID_ITfCompartmentEventSink,
                             static_cast<ITfCompartmentEventSink*>(this), openCloseSinkCookie_);
        }
    }
    if (conversionSinkCookie_ == TF_INVALID_COOKIE) {
        if (auto compartment = ConversionCompartment(threadManager_)) {
            (void)AdviseSink(compartment.Get(), IID_ITfCompartmentEventSink,
                             static_cast<ITfCompartmentEventSink*>(this), conversionSinkCookie_);
        }
    }
    if (profileSinkCookie_ == TF_INVALID_COOKIE) {
        (void)AdviseSink(threadManager_, IID_ITfInputProcessorProfileActivationSink,
                         static_cast<ITfInputProcessorProfileActivationSink*>(this),
                         profileSinkCookie_);
    }

    ComPtr<ITfDocumentMgr> focus;
    ComPtr<ITfContext> top;
    if (SUCCEEDED(threadManager_->GetFocus(focus.Put())) && focus &&
        SUCCEEDED(focus->GetTop(top.Put())) && top) {
        AdviseContextSinks(top.Get());
    }
}

void TextService::UnadviseThreadManagerSinks() {
    UnadviseSink(threadManager_, threadManagerSinkCookie_);
    UnadviseSink(threadManager_, profileSinkCookie_);
    auto compartment = OpenCloseCompartment(threadManager_);
    UnadviseSink(compartment.Get(), openCloseSinkCookie_);
    auto conversion = ConversionCompartment(threadManager_);
    UnadviseSink(conversion.Get(), conversionSinkCookie_);
}

void TextService::AdviseContextSinks(ITfContext* context) {
    if (!context || sinkContext_.Get() == context) return;
    UnadviseContextSinks();
    context->AddRef();
    sinkContext_.Attach(context);
    (void)AdviseSink(context, IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this),
                     textEditSinkCookie_);
    (void)AdviseSink(context, IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this),
                     textLayoutSinkCookie_);
}

void TextService::UnadviseContextSinks() {
    UnadviseSink(sinkContext_.Get(), textEditSinkCookie_);
    UnadviseSink(sinkContext_.Get(), textLayoutSinkCookie_);
    sinkContext_.Reset();
}

void TextService::OpenSettings() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(g_moduleInstance, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        Trace(L"OpenSettings could not resolve the TSF module path");
        return;
    }
    // TEKITO.exe sits beside the DLL, both installed and in a build folder.
    const auto settings =
        std::filesystem::path(modulePath, modulePath + length).parent_path() / L"TEKITO.exe";
    const auto result = ShellExecuteW(nullptr, L"open", settings.c_str(), L"--settings",
                                      settings.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        Trace(L"OpenSettings ShellExecute failed");
    }
}

void TextService::RegisterLangBarItem() {
    if (langBarItemMgr_) return;

    ComPtr<ITfLangBarItemMgr> manager;
    if (!threadManager_ ||
        FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(manager.Put())))) {
        Trace(L"RegisterLangBarItem manager query failed");
        return;
    }

    auto* item = new (std::nothrow) ModeLangBarItem(
        g_moduleInstance,
        [this]() { return mode_; },
        [this](InputMode mode) {
            ChangeInputMode(mode);
            ShowModeIndicator(nullptr);
        },
        [this]() { return ToggledMode(); },
        [this]() { return JapaneseModeAvailable(); },
        [this]() { OpenSettings(); },
        [this]() { return userdata::UseJapaneseUi(userSettings_.uiLanguage); });
    if (!item) return;

    ComPtr<ModeLangBarItem> ownedItem(item);
    if (FAILED(manager->AddItem(ownedItem.Get()))) {
        Trace(L"RegisterLangBarItem AddItem failed");
        return;
    }

    Trace(L"RegisterLangBarItem AddItem succeeded");
    langBarItemMgr_ = std::move(manager);
    modeLangBarItem_ = std::move(ownedItem);
}

void TextService::UnregisterLangBarItem() {
    if (langBarItemMgr_ && modeLangBarItem_) {
        langBarItemMgr_->RemoveItem(modeLangBarItem_.Get());
    }
    modeLangBarItem_.Reset();
    langBarItemMgr_.Reset();
}

HRESULT TextService::OnSetFocus(BOOL foreground) {
    if (foreground) {
        SyncRuntimeState();
    }
    if (!foreground) {
        candidateWindow_.Hide();
    }
    return S_OK;
}

HRESULT TextService::OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    SyncRuntimeState();
    if (IsReadOnly(context)) {
        *eaten = FALSE;
        return S_OK;
    }
    ModeKey modeKey;
    if (TranslateModeKey(wParam, modeKey)) {
        *eaten = TRUE;
        return S_OK;
    }
    KeyInput input{};
    if (!TranslateKey(wParam, lParam, input)) {
        *eaten = FALSE;
        return S_OK;
    }
    if (LetsKeyThrough(input)) {
        // Finish the word now; the key itself goes on to the application.
        if (context) [[maybe_unused]] const auto ignored = RequestKeyEditSession(context, input);
        *eaten = FALSE;
        return S_OK;
    }
    *eaten = TRUE;
    if (*eaten) Trace(L"OnTestKeyDown eaten");
    return S_OK;
}

HRESULT TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) {
    ScopedTraceDuration duration(L"Perf OnKeyDown");
    Trace(L"OnKeyDown");
    if (!context || !eaten) return E_INVALIDARG;
    SyncRuntimeState();
    activationSettling_ = false;
    if (IsReadOnly(context)) {
        *eaten = FALSE;
        return S_OK;
    }
    ModeKey modeKey;
    if (TranslateModeKey(wParam, modeKey)) {
        ApplyModeKey(context, modeKey);
        *eaten = TRUE;
        return S_OK;
    }
    KeyInput input{};
    if (!TranslateKey(wParam, lParam, input)) {
        *eaten = FALSE;
        return S_OK;
    }

    const bool passThrough = LetsKeyThrough(input);
    const HRESULT hr = RequestKeyEditSession(context, input);
    *eaten = hr == S_OK && !passThrough ? TRUE : FALSE;
    if (*eaten) Trace(L"OnKeyDown edit session requested");
    return S_OK;
}

HRESULT TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

HRESULT TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    return S_OK;
}

HRESULT TextService::OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* eaten) {
    if (!eaten) return E_INVALIDARG;
    *eaten = FALSE;
    if (guid != GUID_TekitoToggleKey) return S_OK;
    SyncRuntimeState();
    ModeKey key;
    key.mode = ToggledMode();
    ApplyModeKey(context, key);
    *eaten = TRUE;
    return S_OK;
}

HRESULT TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
HRESULT TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
HRESULT TextService::OnPushContext(ITfContext*) { return S_OK; }

HRESULT TextService::OnPopContext(ITfContext* context) {
    if (context && context == sinkContext_.Get()) UnadviseContextSinks();
    return S_OK;
}

HRESULT TextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr*) {
    // Light or dark taskbar: the mode icon follows when focus moves.
    if (modeLangBarItem_) modeLangBarItem_->RefreshTheme();
    ComPtr<ITfContext> top;
    if (focus && SUCCEEDED(focus->GetTop(top.Put())) && top) {
        AdviseContextSinks(top.Get());
    } else {
        UnadviseContextSinks();
    }

    // A word left behind in a document that lost focus is committed as typed.
    if (compositionContext_) {
        ComPtr<ITfDocumentMgr> owner;
        if (FAILED(compositionContext_->GetDocumentMgr(owner.Put())) || owner.Get() != focus) {
            FinishCompositionLater();
        }
    } else {
        candidateWindow_.Hide();
    }
    return S_OK;
}

HRESULT TextService::OnEndEdit(ITfContext* context, TfEditCookie readOnlyCookie,
                               ITfEditRecord* editRecord) {
    if (!composition_ || !editRecord || context != compositionContext_) return S_OK;
    BOOL selectionChanged = FALSE;
    if (FAILED(editRecord->GetSelectionStatus(&selectionChanged)) || !selectionChanged) {
        return S_OK;
    }

    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(readOnlyCookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched)) ||
        fetched != 1 || !selection.range) {
        return S_OK;
    }
    ComPtr<ITfRange> selectionRange(selection.range);
    ComPtr<ITfRange> compositionRange;
    if (FAILED(composition_->GetRange(compositionRange.Put()))) return S_OK;

    // A click or another edit moved the caret out of the word.
    LONG startOrder = 0;
    LONG endOrder = 0;
    if (SUCCEEDED(selectionRange->CompareStart(readOnlyCookie, compositionRange.Get(),
                                               TF_ANCHOR_START, &startOrder)) &&
        SUCCEEDED(selectionRange->CompareEnd(readOnlyCookie, compositionRange.Get(),
                                             TF_ANCHOR_END, &endOrder)) &&
        (startOrder < 0 || endOrder > 0)) {
        FinishCompositionLater();
    }
    return S_OK;
}

HRESULT TextService::OnLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView*) {
    if (!context || context != compositionContext_ || !candidateWindow_.IsShown()) return S_OK;
    if (code == TF_LC_DESTROY) {
        candidateWindow_.Hide();
        return S_OK;
    }
    KeyInput input{};
    input.type = KeyInput::Type::Reposition;
    auto* session = new (std::nothrow) KeyEditSession(this, context, input);
    if (!session) return S_OK;
    HRESULT sessionResult = E_FAIL;
    (void)context->RequestEditSession(clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READ,
                                      &sessionResult);
    session->Release();
    return S_OK;
}

HRESULT TextService::OnCompositionTerminated(TfEditCookie, ITfComposition* composition) {
    Trace(L"OnCompositionTerminated");
    if (composition_ == composition) {
        composition_->Release();
        composition_ = nullptr;
    }
    if (compositionContext_) {
        compositionContext_->Release();
        compositionContext_ = nullptr;
    }
    candidateWindow_.Hide();
    state_.Reset();
    rawText_.clear();
    japanese_.Clear();
    return S_OK;
}

HRESULT TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) {
    if (!enumInfo) return E_INVALIDARG;
    std::vector<ITfDisplayAttributeInfo*> infos{static_cast<ITfDisplayAttributeInfo*>(this)};
    for (int i = 0; i < kJapaneseUnderlineCount; ++i) {
        if (auto* info = CreateJapaneseUnderline(static_cast<JapaneseUnderline>(i))) infos.push_back(info);
    }
    *enumInfo = new (std::nothrow) DisplayAttributeInfoEnum(infos);
    // The enumerator holds its own references to the underlines made here.
    for (std::size_t i = 1; i < infos.size(); ++i) infos[i]->Release();
    return *enumInfo ? S_OK : E_OUTOFMEMORY;
}

HRESULT TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) {
    if (!info) return E_INVALIDARG;
    *info = nullptr;
    if (guid == GUID_TekitoDisplayAttribute) {
        *info = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    for (int i = 0; i < kJapaneseUnderlineCount; ++i) {
        const auto underline = static_cast<JapaneseUnderline>(i);
        if (guid == JapaneseUnderlineGuid(underline)) {
            *info = CreateJapaneseUnderline(underline);
            return *info ? S_OK : E_OUTOFMEMORY;
        }
    }
    return E_INVALIDARG;
}

HRESULT TextService::GetGUID(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    *guid = GUID_TekitoDisplayAttribute;
    return S_OK;
}

HRESULT TextService::GetDescription(BSTR* description) {
    if (!description) return E_INVALIDARG;
    *description = SysAllocString(L"TEKITO composing underline");
    return *description ? S_OK : E_OUTOFMEMORY;
}

HRESULT TextService::GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) {
    if (!attribute) return E_INVALIDARG;
    *attribute = {};
    attribute->crText.type = TF_CT_NONE;
    attribute->crBk.type = TF_CT_NONE;
    attribute->lsStyle = TF_LS_DOT;
    attribute->fBoldLine = FALSE;
    attribute->crLine.type = TF_CT_SYSCOLOR;
    attribute->crLine.nIndex = COLOR_WINDOWTEXT;
    attribute->bAttr = TF_ATTR_INPUT;
    return S_OK;
}

HRESULT TextService::SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) {
    return E_NOTIMPL;
}

HRESULT TextService::Reset() {
    return S_OK;
}

bool TextService::TranslateKey(WPARAM wParam, LPARAM, KeyInput& input) {
    if (ProcessPassesThrough() || FocusIsClassicPasswordEdit()) return false;
    if (mode_ == InputMode::Japanese) {
        if (TranslateEnglishSegmentKey(wParam, input)) return true;
        return TranslateJapaneseKey(wParam, input);
    }
    if (state_.Mode() == InputMode::Direct) return false;
    return TranslateEnglishKey(wParam, input);
}

bool TextService::TranslateEnglishKey(WPARAM wParam, KeyInput& input) {
    if (!EnsureEngine()) return false;  // language data still loading

    if (HasBlockedModifier()) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }

    if (wParam == VK_SPACE) {
        if (!state_.IsActive()) return false;
        input.type = (GetKeyState(VK_SHIFT) & 0x8000) ? KeyInput::Type::ShiftSpace
                                                     : KeyInput::Type::Space;
        return true;
    }
    if (wParam == VK_TAB) {
        if (!state_.IsActive()) return false;
        input.type = (GetKeyState(VK_SHIFT) & 0x8000) ? KeyInput::Type::ShiftTab
                                                     : KeyInput::Type::Tab;
        return true;
    }
    if (wParam == VK_BACK) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::Backspace;
        return true;
    }
    if (wParam == VK_ESCAPE) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::Cancel;
        return true;
    }
    if (wParam == VK_RETURN) {
        if (!state_.IsActive() && !userSettings_.periodOnEnter) return false;
        input.type = KeyInput::Type::Enter;
        return true;
    }
    if ((state_.IsCandidateNavigationActive() || state_.State() == CompositionState::Cycling) &&
        (wParam == VK_UP || wParam == VK_DOWN)) {
        input.type = wParam == VK_UP ? KeyInput::Type::UpArrow : KeyInput::Type::DownArrow;
        return true;
    }
    if (IsNavigationKey(wParam)) {
        if (!state_.IsActive()) return false;
        input.type = KeyInput::Type::EndComposition;
        return true;
    }

    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState)) return false;
    wchar_t buffer[8]{};
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(wParam), MAPVK_VK_TO_VSC);
    const int count = ToUnicodeEx(static_cast<UINT>(wParam), scanCode, keyboardState,
                                  buffer, static_cast<int>(std::size(buffer)), 0,
                                  GetKeyboardLayout(0));
    if (count == 1 && buffer[0] >= 0x20 && buffer[0] != 0x7f) {
        PunctuationRole punctuationRole{};
        if (TryClassifyPunctuation(buffer[0], punctuationRole)) {
            if (!state_.IsActive()) return false;
            input.type = KeyInput::Type::Punctuation;
            input.character = buffer[0];
            input.punctuationRole = punctuationRole;
            return true;
        }
        if (!IsWordCharacter(buffer[0])) {
            // Digits and symbols are not part of a word: finish the word
            // being typed as it stands and let the character through.
            if (!state_.IsActive()) return false;
            input.type = KeyInput::Type::EndComposition;
            return true;
        }
        if (!state_.IsActive() && !std::iswalpha(buffer[0])) return false;  // no word starts with '
        input.type = KeyInput::Type::Printable;
        input.character = buffer[0];
        return true;
    }

    return false;
}

HRESULT TextService::RequestKeyEditSession(ITfContext* context, const KeyInput& input) {
    auto* session = new (std::nothrow) KeyEditSession(this, context, input);
    if (!session) return E_OUTOFMEMORY;

    HRESULT sessionResult = E_FAIL;
    HRESULT hr = context->RequestEditSession(clientId_, session,
                                             TF_ES_SYNC | TF_ES_READWRITE,
                                             &sessionResult);
    const bool guardedTarget = ProcessPassesThrough() || FocusIsClassicPasswordEdit();
    const bool syncRejected = hr == TF_E_SYNCHRONOUS || sessionResult == TF_E_SYNCHRONOUS ||
                              (hr == E_UNEXPECTED && sessionResult == E_FAIL);
    const bool retryAsync = !guardedTarget && syncRejected;
    if (retryAsync) {
        Trace(L"RequestEditSession retry async");
        sessionResult = E_FAIL;
        hr = context->RequestEditSession(clientId_, session,
                                         TF_ES_ASYNC | TF_ES_READWRITE,
                                         &sessionResult);
        if (SUCCEEDED(hr)) {
            session->Release();
            return S_OK;
        }
    }
    session->Release();
    if (FAILED(hr) || FAILED(sessionResult)) {
        TraceHr(L"RequestEditSession failed", FAILED(hr) ? hr : sessionResult);
    }
    if (FAILED(hr)) return hr;
    if (FAILED(sessionResult)) return sessionResult;
    return sessionResult;
}

void TextService::OnCandidateSelected(std::size_t index) {
    if (!compositionContext_ || (!state_.IsActive() && !japanese_.IsConverted())) return;
    KeyInput input{};
    input.type = KeyInput::Type::CandidateSelection;
    input.candidateIndex = index;
    [[maybe_unused]] const auto ignored = RequestKeyEditSession(compositionContext_, input);
}

HRESULT TextService::HandleKeyInEditSession(ITfContext* context, TfEditCookie editCookie,
                                            const KeyInput& input) {
    const HRESULT hr = HandleKeyInEditSessionCore(context, editCookie, input);
    // An English word typed with Shift in Japanese ends once it is committed.
    if (englishSegment_ && !state_.IsActive()) EndEnglishSegment();
    return hr;
}

HRESULT TextService::HandleKeyInEditSessionCore(ITfContext* context, TfEditCookie editCookie,
                                                const KeyInput& input) {
    Trace(L"HandleKeyInEditSession");
    if (!context) return E_INVALIDARG;
    if (input.type == KeyInput::Type::ShowModeIndicator) {
        ShowModeBadge(CaretAnchor(context, editCookie));
        return S_OK;
    }
    if (input.type == KeyInput::Type::Reposition) {
        if (!composition_ || !candidateWindow_.IsShown()) return S_OK;
        // Japanese conversion candidates and predictions follow the text too.
        if (japanese_.IsComposing()) {
            ShowJapaneseCandidates(context, editCookie);
            return S_OK;
        }
        const RECT anchor = GetCandidateAnchor(context, editCookie);
        if (!EqualRect(&anchor, &candidateAnchor_)) {
            candidateAnchor_ = anchor;
            candidateWindow_.Show(anchor, EnglishRows(), state_.SelectedIndex(), state_.PageStart(),
                                  state_.VisibleCount(), SelectedEnglishMeaning());
        }
        return S_OK;
    }
    if (input.type == KeyInput::Type::EnglishSegmentEnd ||
        input.type == KeyInput::Type::EnglishSegmentCommit) {
        return HandleEnglishSegmentEnd(context, editCookie, input);
    }
    if (input.englishSegment && !englishSegment_ && mode_ == InputMode::Japanese) {
        englishSegment_ = true;
        state_.SetInputMode(InputMode::Convert);
    }
    // Japanese typing, and committing it after the mode or focus changed.
    if ((mode_ == InputMode::Japanese && !englishSegment_) ||
        input.type == KeyInput::Type::JapaneseCommit || japanese_.IsComposing()) {
        if (input.type != KeyInput::Type::JapaneseCommit &&
            (ContextHasPassThroughInputScope(context, editCookie) || FocusIsClassicPasswordEdit() ||
             ProcessPassesThrough())) {
            const HRESULT hr = CommitJapanese(context, editCookie);
            return SUCCEEDED(hr) ? S_FALSE : hr;
        }
        return HandleJapaneseKey(context, editCookie, input);
    }
    if (state_.Mode() == InputMode::Direct) {
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        const HRESULT hr = EndComposition(editCookie);
        return SUCCEEDED(hr) ? S_FALSE : hr;
    }
    if (ContextHasPassThroughInputScope(context, editCookie) || FocusIsClassicPasswordEdit() ||
        ProcessPassesThrough()) {
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        HRESULT hr = EndComposition(editCookie);
        return SUCCEEDED(hr) ? S_FALSE : hr;
    }

    const auto recordSocialSelection = [&](const Candidate& candidate) {
        if (userSettings_.socialPersonalization <= 0 ||
            candidate.socialRange == SocialRangeUnspecified) return;
        const auto trigger = rawText_.starts_with(L':') ? std::wstring_view(rawText_).substr(1)
                                                        : std::wstring_view(rawText_);
        socialLearning_.RecordSelection(trigger, candidate.text);
        for (const auto& other : state_.Candidates()) {
            if (&other != &candidate &&
                other.socialRange != SocialRangeUnspecified) {
                socialLearning_.RecordNonSelection(trigger, other.text);
            }
        }
    };
    const auto recordLearningDecision = [&](std::wstring_view raw,
                                            const std::vector<Candidate>& candidates,
                                            std::size_t selectedIndex,
                                            bool explicitSelection) {
        if (!userSettings_.learningEnabled || raw.empty() || selectedIndex >= candidates.size()) {
            return;
        }
        const auto& selected = candidates[selectedIndex];
        if (selected.text.empty()) return;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (candidate.text.empty()) continue;
            userLearning_.RecordExposure(raw, candidate.text);
            if (index != selectedIndex) userLearning_.RecordNonSelection(raw, candidate.text);
        }
        if (explicitSelection) {
            userLearning_.RecordSelection(raw, selected.text);
        } else {
            userLearning_.RecordAutomaticAcceptance(raw, selected.text);
        }
    };

    switch (input.type) {
    case KeyInput::Type::Printable: {
        if (!composition_ && !state_.IsActive() &&
            !CanStartWordAfter(ReadSelectionContext(context, editCookie))) {
            // Inside a URL, address, path or identifier: type it as is.
            return InsertAtSelection(context, editCookie, std::wstring(1, input.character));
        }
        const auto action = state_.OnPrintable();
        if (action.kind == ActionKind::CommitAndStartNext) {
            HRESULT hr = EndComposition(editCookie);
            if (FAILED(hr)) return hr;
            candidateWindow_.Hide();
            rawText_.clear();
        }

        HRESULT hr = EnsureComposition(context, editCookie);
        if (FAILED(hr)) return hr;

        auto character = input.character;
        if (rawText_.empty()) {
            Context conversionContext;
            ReadCompositionContext(composition_, editCookie,
                                   conversionContext.precedingText,
                                   conversionContext.followingText);
            sentenceStart_ = IsSentenceStart(conversionContext.precedingText);
            capitalizationOrigin_ = CapitalizationOrigin::UserTyped;
            if (sentenceStart_ && std::iswlower(character)) {
                character = static_cast<wchar_t>(std::towupper(character));
                capitalizationOrigin_ = CapitalizationOrigin::EngineApplied;
            }
        } else if (capitalizationOrigin_ == CapitalizationOrigin::EngineApplied &&
                   rawText_.size() == 1 && std::iswupper(character)) {
            rawText_.front() = static_cast<wchar_t>(std::towlower(rawText_.front()));
            capitalizationOrigin_ = CapitalizationOrigin::UserTyped;
        }
        rawText_.push_back(character);
        hr = ReplaceComposition(context, editCookie, rawText_);
        if (FAILED(hr)) return hr;
        RefreshCandidates(context, editCookie);
        Trace(L"Printable handled");
        return S_OK;
    }

    case KeyInput::Type::Space: {
        const auto rawBefore = rawText_;
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool explicitSelection = state_.IsCandidateNavigationActive();
        if (explicitSelection && selectedBefore < state_.Candidates().size()) {
            recordLearningDecision(rawBefore, state_.Candidates(), selectedBefore, true);
            recordSocialSelection(state_.Candidates()[selectedBefore]);
        }
        const auto action = state_.OnSpace();
        if (action.kind == ActionKind::CommitAndStartNext) {
            if (userSettings_.learningEnabled) {
                if (explicitSelection && !candidateBefore.empty() && candidateBefore != rawBefore) {
                    userLearning_.RecordCandidateSelection(candidateBefore);
                } else {
                    userLearning_.RecordRawKeep(rawBefore);
                }
            }
            HRESULT hr = ReplaceComposition(context, editCookie, action.text);
            if (FAILED(hr)) return hr;
            hr = EndComposition(editCookie);
            if (FAILED(hr)) return hr;
            candidateWindow_.Hide();
            rawText_.clear();
            return S_OK;
        }
        if (action.kind != ActionKind::ReplaceComposition) return S_FALSE;
        const auto selectedAfter = state_.SelectedIndex();
        if (selectedAfter < state_.Candidates().size()) {
            if (explicitSelection) recordSocialSelection(state_.Candidates()[selectedAfter]);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (SUCCEEDED(hr)) {
            if (action.showCandidates) {
                ShowCandidates(context, editCookie);
            } else {
                candidateWindow_.Hide();
            }
        }
        return hr;
    }

    case KeyInput::Type::ShiftSpace:
    case KeyInput::Type::Tab:
    case KeyInput::Type::ShiftTab:
    case KeyInput::Type::UpArrow:
    case KeyInput::Type::DownArrow:
    case KeyInput::Type::CandidateSelection: {
        InputAction action;
        if (input.type == KeyInput::Type::CandidateSelection) {
            if (input.candidateIndex < state_.Candidates().size()) {
                recordSocialSelection(state_.Candidates()[input.candidateIndex]);
            }
            action = state_.OnCandidateSelected(input.candidateIndex);
        } else if (input.type == KeyInput::Type::ShiftSpace ||
                   input.type == KeyInput::Type::ShiftTab ||
                   input.type == KeyInput::Type::UpArrow) {
            action = state_.OnPreviousCandidate();
        } else {
            action = state_.OnNextCandidate();
        }
        if (action.kind != ActionKind::ReplaceComposition) return S_FALSE;
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (SUCCEEDED(hr)) ShowCandidates(context, editCookie);
        return hr;
    }

    case KeyInput::Type::Backspace: {
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool socialCandidateBefore = selectedBefore < state_.Candidates().size() &&
            state_.Candidates()[selectedBefore].socialRange != SocialRangeUnspecified;
        const auto action = state_.OnBackspace();
        if (action.kind == ActionKind::RestoreOriginal) {
            if (userSettings_.learningEnabled && !candidateBefore.empty()) userLearning_.RecordUndo(candidateBefore);
            if (userSettings_.learningEnabled && !candidateBefore.empty()) {
                userLearning_.RecordExposure(rawText_, candidateBefore);
                userLearning_.RecordUndo(rawText_, candidateBefore);
            }
            if (socialCandidateBefore && userSettings_.socialPersonalization > 0) {
                const auto trigger = rawText_.starts_with(L':') ? std::wstring_view(rawText_).substr(1)
                                                                : std::wstring_view(rawText_);
                socialLearning_.RecordUndo(trigger, candidateBefore);
            }
            rawText_ = action.text;
            HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
            if (SUCCEEDED(hr)) ShowCandidates(context, editCookie);
            return hr;
        }

        if (!rawText_.empty()) rawText_.pop_back();
        if (rawText_.empty()) {
            HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
            if (FAILED(hr)) return hr;
            state_.Reset();
            candidateWindow_.Hide();
            return EndComposition(editCookie);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, rawText_);
        if (SUCCEEDED(hr)) RefreshCandidates(context, editCookie);
        return hr;
    }

    case KeyInput::Type::Punctuation: {
        const auto rawBefore = rawText_;
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool explicitSelection = state_.IsCandidateNavigationActive();
        if (selectedBefore < state_.Candidates().size()) {
            recordLearningDecision(rawBefore, state_.Candidates(), selectedBefore, explicitSelection);
        }
        const auto action = state_.OnPunctuation(input.character, input.punctuationRole);
        if (action.kind != ActionKind::ReplaceComposition) return S_FALSE;
        if (!candidateBefore.empty() && candidateBefore != rawBefore) {
            if (userSettings_.learningEnabled) userLearning_.RecordCandidateSelection(candidateBefore);
        } else {
            if (userSettings_.learningEnabled) userLearning_.RecordRawKeep(rawBefore);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (FAILED(hr)) return hr;
        hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        rawText_.clear();
        return S_OK;
    }

    case KeyInput::Type::Cancel: {
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const auto action = state_.OnCancel();
        if (action.kind != ActionKind::ReplaceComposition &&
            action.kind != ActionKind::RestoreOriginal) return S_FALSE;
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (FAILED(hr)) return hr;
        if (action.kind == ActionKind::RestoreOriginal) {
            if (userSettings_.learningEnabled && !candidateBefore.empty()) {
                userLearning_.RecordExposure(rawText_, candidateBefore);
                userLearning_.RecordUndo(rawText_, candidateBefore);
            }
            rawText_ = action.text;
            ShowCandidates(context, editCookie);
            return S_OK;
        }
        hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        rawText_.clear();
        return S_OK;
    }

    case KeyInput::Type::Enter: {
        if (!state_.IsActive()) {
            if (!userSettings_.periodOnEnter) return S_FALSE;
            const auto preceding = ReadSelectionContext(context, editCookie);
            if (!NeedsTerminalPeriod(preceding)) return S_FALSE;
            std::size_t trailingWhitespace = 0;
            while (trailingWhitespace < preceding.size()) {
                const auto ch = preceding[preceding.size() - trailingWhitespace - 1];
                if (ch != L' ' && ch != L'\t') break;
                ++trailingWhitespace;
            }
            return trailingWhitespace == 0
                       ? InsertAtSelection(context, editCookie, L".")
                       : ReplaceTrailingWhitespaceAtSelection(context, editCookie,
                                                              trailingWhitespace, L".");
        }
        const auto rawBefore = rawText_;
        const auto selectedBefore = state_.SelectedIndex();
        const auto candidateBefore = selectedBefore < state_.Candidates().size()
                                         ? state_.Candidates()[selectedBefore].text
                                         : std::wstring{};
        const bool explicitSelection = state_.IsCandidateNavigationActive();
        if (selectedBefore < state_.Candidates().size()) {
            recordLearningDecision(rawBefore, state_.Candidates(), selectedBefore, explicitSelection);
        }
        const auto action = state_.OnEnter(userSettings_.periodOnEnter);
        if (action.kind != ActionKind::CommitBeforeNewline &&
            action.kind != ActionKind::EndComposition) return S_FALSE;
        if (!candidateBefore.empty() && candidateBefore != rawBefore) {
            if (userSettings_.learningEnabled) userLearning_.RecordCandidateSelection(candidateBefore);
        } else {
            if (userSettings_.learningEnabled) userLearning_.RecordRawKeep(rawBefore);
        }
        HRESULT hr = ReplaceComposition(context, editCookie, action.text);
        if (FAILED(hr)) return hr;
        hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        rawText_.clear();
        return S_OK;
    }

    case KeyInput::Type::EndComposition: {
        HRESULT hr = EndComposition(editCookie);
        if (FAILED(hr)) return hr;
        candidateWindow_.Hide();
        state_.Reset();
        rawText_.clear();
        return S_OK;
    }
    }

    return E_UNEXPECTED;
}

HRESULT TextService::EnsureComposition(ITfContext* context, TfEditCookie editCookie) {
    if (composition_) return S_OK;

    ComPtr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(insertAtSelection.Put()));
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition QueryInterface ITfInsertAtSelection failed", hr);
        return hr;
    }

    ComPtr<ITfRange> range;
    hr = insertAtSelection->InsertTextAtSelection(editCookie, TF_IAS_QUERYONLY,
                                                  nullptr, 0, range.Put());
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition InsertTextAtSelection query failed", hr);
        return hr;
    }

    ComPtr<ITfContextComposition> compositionContext;
    hr = context->QueryInterface(IID_PPV_ARGS(compositionContext.Put()));
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition QueryInterface ITfContextComposition failed", hr);
        return hr;
    }

    hr = compositionContext->StartComposition(editCookie, range.Get(), this, &composition_);
    if (FAILED(hr)) {
        TraceHr(L"EnsureComposition StartComposition failed", hr);
        return hr;
    }

    compositionContext_ = context;
    compositionContext_->AddRef();
    AdviseContextSinks(context);
    return S_OK;
}

HRESULT TextService::ReplaceComposition(ITfContext* context, TfEditCookie editCookie,
                                        const std::wstring& text) {
    if (!composition_) return E_UNEXPECTED;
    ComPtr<ITfRange> range;
    HRESULT hr = composition_->GetRange(range.Put());
    if (FAILED(hr)) {
        TraceHr(L"ReplaceComposition GetRange failed", hr);
        return hr;
    }
    hr = range->SetText(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    if (FAILED(hr)) {
        TraceHr(L"ReplaceComposition SetText failed", hr);
        return hr;
    }

    if (!text.empty()) {
        hr = ApplyDisplayAttribute(context, editCookie, range.Get());
        if (FAILED(hr)) {
            TraceHr(L"ReplaceComposition ApplyDisplayAttribute failed", hr);
            return hr;
        }
    }

    hr = range->Collapse(editCookie, TF_ANCHOR_END);
    if (FAILED(hr)) {
        TraceHr(L"ReplaceComposition Collapse failed", hr);
        return hr;
    }

    TF_SELECTION selection{};
    selection.range = range.Get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    return context->SetSelection(editCookie, 1, &selection);
}

HRESULT TextService::ApplyDisplayAttribute(ITfContext* context, TfEditCookie editCookie,
                                           ITfRange* range, REFGUID attribute) {
    std::size_t slot = 0;
    for (int i = 0; i < kJapaneseUnderlineCount; ++i) {
        if (attribute == JapaneseUnderlineGuid(static_cast<JapaneseUnderline>(i))) slot = i + 1;
    }
    TfGuidAtom& atom = displayAttributeAtoms_[slot];
    if (atom == TF_INVALID_GUIDATOM) {
        ComPtr<ITfCategoryMgr> categoryManager;
        HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(categoryManager.Put()));
        if (FAILED(hr)) {
            TraceHr(L"ApplyDisplayAttribute CoCreateInstance category manager failed", hr);
            return hr;
        }
        hr = categoryManager->RegisterGUID(attribute, &atom);
        if (FAILED(hr)) {
            TraceHr(L"ApplyDisplayAttribute RegisterGUID failed", hr);
            return hr;
        }
    }

    ComPtr<ITfProperty> property;
    HRESULT hr = context->GetProperty(GUID_PROP_ATTRIBUTE, property.Put());
    if (FAILED(hr)) {
        TraceHr(L"ApplyDisplayAttribute GetProperty attribute failed", hr);
        return hr;
    }

    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = static_cast<LONG>(atom);
    return property->SetValue(editCookie, range, &value);
}

HRESULT TextService::EndComposition(TfEditCookie editCookie) {
    if (!composition_) return S_OK;
    ITfComposition* composition = composition_;
    composition->AddRef();
    HRESULT hr = composition->EndComposition(editCookie);
    if (composition_ == composition) {
        composition_->Release();
        composition_ = nullptr;
        if (compositionContext_) {
            compositionContext_->Release();
            compositionContext_ = nullptr;
        }
    }
    composition->Release();
    return hr;
}

HRESULT TextService::InsertAtSelection(ITfContext* context, TfEditCookie editCookie,
                                       const std::wstring& text) {
    ComPtr<ITfInsertAtSelection> insertAtSelection;
    HRESULT hr = context->QueryInterface(IID_PPV_ARGS(insertAtSelection.Put()));
    if (FAILED(hr)) return hr;

    ComPtr<ITfRange> insertedRange;
    hr = insertAtSelection->InsertTextAtSelection(editCookie, 0, text.c_str(), static_cast<LONG>(text.size()),
                                                  insertedRange.Put());
    if (FAILED(hr) || !insertedRange) return hr;
    // Some applications leave the caret before the inserted text; typing on
    // must go after it (a space, then the next word).
    if (FAILED(insertedRange->Collapse(editCookie, TF_ANCHOR_END))) return S_OK;
    TF_SELECTION selection{};
    selection.range = insertedRange.Get();
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    (void)context->SetSelection(editCookie, 1, &selection);
    return S_OK;
}

void TextService::RefreshCandidates(ITfContext* context, TfEditCookie editCookie) {
    ScopedTraceDuration totalDuration(L"Perf RefreshCandidates");
    if (!candidateEngine_) {
        state_.Reset();
        candidateWindow_.Hide();
        return;
    }
    Context conversionContext;
    ReadCompositionContext(composition_, editCookie, conversionContext.precedingText,
                            conversionContext.followingText);
    ConversionRequest request;
    request.rawText = rawText_;
    request.context = std::move(conversionContext);
    request.context.sentenceStart = sentenceStart_;
    request.capitalization.origin = capitalizationOrigin_;
    request.options.correctionEnabled = userSettings_.correctionEnabled;
    request.options.commonMisspellingsEnabled = userSettings_.commonMisspellingsEnabled;
    request.options.contextSuggestionsEnabled = userSettings_.contextSuggestionsEnabled;
    request.options.completionEnabled = userSettings_.completionEnabled;
    request.options.japanesePhoneticSuggestionsEnabled =
        userSettings_.japanesePhoneticSuggestionsEnabled;
    request.options.socialExpressionRange = userSettings_.socialExpressionRange;
    request.options.special = SpecialOptions();
    {
        ScopedTraceDuration duration(L"Perf CandidateEngine");
        auto result = candidateEngine_->Convert(request);
        state_.BeginOrUpdate(rawText_, std::move(result.candidates));
    }
    ScopedTraceDuration duration(L"Perf CandidateWindow");
    ShowCandidates(context, editCookie);
}

void TextService::ShowCandidates(ITfContext* context, TfEditCookie editCookie) {
    if (!userSettings_.candidateWindowEnabled) {
        candidateWindow_.Hide();
        return;
    }
    const RECT anchor = GetCandidateAnchor(context, editCookie);
    candidateAnchor_ = anchor;
    candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
    candidateWindow_.SetJapanese(userdata::UseJapaneseUi(userSettings_.uiLanguage));
    candidateWindow_.Show(anchor, EnglishRows(), state_.SelectedIndex(), state_.PageStart(), state_.VisibleCount(),
                          SelectedEnglishMeaning());
    if (userSettings_.socialPersonalization > 0) {
        const auto trigger = rawText_.starts_with(L':') ? std::wstring_view(rawText_).substr(1)
                                                        : std::wstring_view(rawText_);
        for (const auto& candidate : state_.Candidates()) {
            if (candidate.socialRange != SocialRangeUnspecified) {
                socialLearning_.RecordExposure(trigger, candidate.text);
            }
        }
    }
}

RECT TextService::CaretAnchor(ITfContext* context, TfEditCookie editCookie) const {
    if (context && !composition_) {
        TF_SELECTION selection{};
        ULONG fetched = 0;
        if (SUCCEEDED(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) &&
            fetched == 1 && selection.range) {
            ComPtr<ITfContextView> view;
            RECT rect{};
            BOOL clipped = FALSE;
            const bool found = SUCCEEDED(context->GetActiveView(view.Put())) &&
                               SUCCEEDED(view->GetTextExt(editCookie, selection.range, &rect, &clipped)) &&
                               rect.bottom > rect.top;
            selection.range->Release();
            if (found) return rect;
        }
    }
    return GetCandidateAnchor(composition_ ? context : nullptr, editCookie);
}

void TextService::ShowModeIndicator(ITfContext* context) {
    if (!userSettings_.modeIndicatorEnabled || ProcessPassesThrough()) return;
    ComPtr<ITfContext> focused;
    if (!context && threadManager_) {
        ComPtr<ITfDocumentMgr> document;
        if (SUCCEEDED(threadManager_->GetFocus(document.Put())) && document) {
            (void)document->GetTop(focused.Put());
        }
        context = focused.Get();
    }
    if (context) {
        KeyInput input{};
        input.type = KeyInput::Type::ShowModeIndicator;
        if (auto* session = new (std::nothrow) KeyEditSession(this, context, input)) {
            HRESULT sessionResult = E_FAIL;
            const HRESULT hr = context->RequestEditSession(clientId_, session, TF_ES_ASYNCDONTCARE | TF_ES_READ,
                                                           &sessionResult);
            session->Release();
            if (SUCCEEDED(hr)) return;
        }
    }
    // No document to ask: the system caret, or the pointer.
    ShowModeBadge(GetCandidateAnchor(nullptr, 0));
}

void TextService::ShowModeBadge(const RECT& caret) {
    candidateWindow_.SetStyle(userSettings_.candidateWindowStyle);
    candidateWindow_.ShowModeBadge(caret, mode_);
}

RECT TextService::GetCandidateAnchor(ITfContext* context, TfEditCookie editCookie) const {
    RECT rect{};
    BOOL clipped = FALSE;
    if (context && composition_) {
        ComPtr<ITfContextView> view;
        ComPtr<ITfRange> range;
        if (SUCCEEDED(context->GetActiveView(view.Put())) &&
            SUCCEEDED(composition_->GetRange(range.Put())) &&
            SUCCEEDED(view->GetTextExt(editCookie, range.Get(), &rect, &clipped)) &&
            (rect.right > rect.left || rect.bottom > rect.top)) {
            return rect;
        }
    }

    // Some hosts fail GetTextExt or report an empty rect. The system caret is
    // a far better anchor than the mouse pointer, which can be anywhere.
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(GetForegroundWindow(), nullptr), &info) &&
        info.hwndCaret &&
        (info.rcCaret.right > info.rcCaret.left || info.rcCaret.bottom > info.rcCaret.top)) {
        POINT topLeft{info.rcCaret.left, info.rcCaret.top};
        POINT bottomRight{info.rcCaret.right, info.rcCaret.bottom};
        ClientToScreen(info.hwndCaret, &topLeft);
        ClientToScreen(info.hwndCaret, &bottomRight);
        return {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    }

    POINT point{};
    GetCursorPos(&point);
    rect.left = rect.right = point.x;
    rect.top = rect.bottom = point.y;
    return rect;
}

void TextService::CheckJapaneseContext(ITfContext* context, TfEditCookie editCookie) {
    if (lastJapaneseCommit_.empty() || !ReadSelectionContext(context, editCookie).ends_with(lastJapaneseCommit_)) {
        japanese_.ForgetContext();
    }
}

}  // namespace tekito::tsf
