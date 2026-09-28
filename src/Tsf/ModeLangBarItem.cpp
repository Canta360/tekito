#include "Tsf/ModeLangBarItem.h"

#include "Tsf/Diagnostics.h"
#include "Tsf/TekitoGuids.h"
#include "assets/tekito_resource.h"

#include <ctffunc.h>
#include <oleauto.h>
#include <cwchar>

namespace tekito::tsf {
namespace {

constexpr UINT kMenuAuto = 1;
constexpr UINT kMenuDirect = 2;
constexpr UINT kMenuSettings = 3;
constexpr UINT kMenuJapanese = 4;

// Whether the taskbar, where the mode icon shows, is dark (Settings >
// Personalization > Colors > "Choose your default Windows mode").
bool TaskbarIsDark() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const auto result =
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return result == ERROR_SUCCESS && value == 0;
}

}  // namespace

ModeLangBarItem::ModeLangBarItem(HINSTANCE instance, ModeGetter getMode, ModeSetter setMode,
                                 ModeGetter toggledMode, Query japaneseProfile,
                                 SettingsLauncher launchSettings, Query japaneseUi)
    : instance_(instance),
      getMode_(std::move(getMode)),
      setMode_(std::move(setMode)),
      toggledMode_(std::move(toggledMode)),
      japaneseProfile_(std::move(japaneseProfile)),
      launchSettings_(std::move(launchSettings)),
      japaneseUi_(std::move(japaneseUi)) {}

const wchar_t* ModeLangBarItem::Text(const wchar_t* english, const wchar_t* japanese) const {
    return japaneseUi_ && japaneseUi_() ? japanese : english;
}

ModeLangBarItem::~ModeLangBarItem() {
    if (sink_) sink_->Release();
}

HRESULT ModeLangBarItem::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem ||
        riid == IID_ITfLangBarItemButton) {
        *object = static_cast<ITfLangBarItemButton*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_ITfSource) {
        *object = static_cast<ITfSource*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ModeLangBarItem::AddRef() { return ++refCount_; }

ULONG ModeLangBarItem::Release() {
    const ULONG value = --refCount_;
    if (value == 0) delete this;
    return value;
}

HRESULT ModeLangBarItem::GetInfo(TF_LANGBARITEMINFO* info) {
    if (!info) return E_INVALIDARG;
    Trace(L"ModeLangBarItem GetInfo");
    *info = {};
    info->clsidService = CLSID_TekitoTextService;
    info->guidItem = GUID_LBI_INPUTMODE;
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU;
    info->ulSort = 0;
    wcscpy_s(info->szDescription, Text(L"TEKITO Mode", L"TEKITO のモード"));
    return S_OK;
}

HRESULT ModeLangBarItem::GetStatus(DWORD* status) {
    if (!status) return E_INVALIDARG;
    *status = 0;
    return S_OK;
}

HRESULT ModeLangBarItem::Show(BOOL) { return E_NOTIMPL; }

HRESULT ModeLangBarItem::GetTooltipString(BSTR* tooltip) {
    if (!tooltip) return E_INVALIDARG;
    const auto mode = Mode();
    *tooltip = SysAllocString(mode == InputMode::Japanese ? Text(L"TEKITO Japanese", L"TEKITO 日本語")
                              : mode == InputMode::Direct
                                  ? Text(L"TEKITO Direct mode", L"TEKITO Direct モード")
                                  : Text(L"TEKITO Auto mode", L"TEKITO Auto モード"));
    return *tooltip ? S_OK : E_OUTOFMEMORY;
}

HRESULT ModeLangBarItem::OnClick(TfLBIClick click, POINT point, const RECT*) {
    if (click == TF_LBI_CLK_RIGHT) {
        ShowContextMenu(point);
        return S_OK;
    }
    if (click != TF_LBI_CLK_LEFT || !setMode_ || !toggledMode_) return S_OK;
    Trace(L"ModeLangBarItem OnClick");
    setMode_(toggledMode_());
    NotifyUpdate();
    return S_OK;
}

void ModeLangBarItem::ShowContextMenu(POINT point) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const auto flags = [this](InputMode mode) {
        return MF_STRING | (Mode() == mode ? MF_CHECKED : 0);
    };
    AppendMenuW(menu, flags(InputMode::Convert), kMenuAuto, L"Auto");
    AppendMenuW(menu, flags(InputMode::Direct), kMenuDirect, L"Direct");
    if (JapaneseProfile()) {
        AppendMenuW(menu, flags(InputMode::Japanese), kMenuJapanese, Text(L"Japanese", L"日本語"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, Text(L"Settings...", L"設定..."));

    const HWND owner = GetForegroundWindow();
    if (owner) SetForegroundWindow(owner);
    const UINT id = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                                   point.x, point.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (id != 0) OnMenuSelect(id);
}

HRESULT ModeLangBarItem::InitMenu(ITfMenu* menu) {
    if (!menu) return E_INVALIDARG;
    const auto mode = Mode();
    const auto addItem = [menu](UINT id, DWORD flags, const wchar_t* label) {
        return menu->AddMenuItem(id, flags, nullptr, nullptr, label,
                                 label ? static_cast<ULONG>(wcslen(label)) : 0, nullptr);
    };
    const auto checked = [mode](InputMode item) {
        return mode == item ? static_cast<DWORD>(TF_LBMENUF_RADIOCHECKED) : 0;
    };
    HRESULT hr = addItem(kMenuAuto, checked(InputMode::Convert), L"Auto");
    if (FAILED(hr)) return hr;
    hr = addItem(kMenuDirect, checked(InputMode::Direct), L"Direct");
    if (FAILED(hr)) return hr;
    if (JapaneseProfile()) {
        hr = addItem(kMenuJapanese, checked(InputMode::Japanese), Text(L"Japanese", L"日本語"));
        if (FAILED(hr)) return hr;
    }
    hr = addItem(0, TF_LBMENUF_SEPARATOR, nullptr);
    if (FAILED(hr)) return hr;
    return addItem(kMenuSettings, 0, Text(L"Settings...", L"設定..."));
}

HRESULT ModeLangBarItem::OnMenuSelect(UINT id) {
    if (id == kMenuSettings) {
        if (launchSettings_) launchSettings_();
        return S_OK;
    }
    if (!setMode_) return S_OK;
    if (id == kMenuAuto) {
        setMode_(InputMode::Convert);
    } else if (id == kMenuDirect) {
        setMode_(InputMode::Direct);
    } else if (id == kMenuJapanese && JapaneseProfile()) {
        setMode_(InputMode::Japanese);
    } else {
        return S_OK;
    }
    NotifyUpdate();
    return S_OK;
}

HRESULT ModeLangBarItem::GetIcon(HICON* icon) {
    if (!icon) return E_INVALIDARG;
    const auto mode = Mode();
    // Auto's icon reads on either taskbar; the others have a dark version.
    darkTaskbar_ = TaskbarIsDark();
    const int resourceId = mode == InputMode::Japanese ? (darkTaskbar_ ? IDI_JAPANESE_DARK : IDI_JAPANESE)
                           : mode == InputMode::Direct ? (darkTaskbar_ ? IDI_DIRECT_DARK : IDI_DIRECT)
                                                       : IDI_AUTO;
    const int size = GetSystemMetrics(SM_CXSMICON);
    *icon = static_cast<HICON>(
        LoadImageW(instance_, MAKEINTRESOURCEW(resourceId), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    Trace(*icon ? L"ModeLangBarItem GetIcon succeeded" : L"ModeLangBarItem GetIcon failed");
    return *icon ? S_OK : E_FAIL;
}

HRESULT ModeLangBarItem::GetText(BSTR* text) {
    if (!text) return E_INVALIDARG;
    Trace(L"ModeLangBarItem GetText");
    const auto mode = Mode();
    *text = SysAllocString(mode == InputMode::Japanese ? L"あ"
                           : mode == InputMode::Direct ? L"D"
                                                       : L"A");
    return *text ? S_OK : E_OUTOFMEMORY;
}

HRESULT ModeLangBarItem::AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) {
    if (!cookie) return E_INVALIDARG;
    *cookie = 0;
    if (riid != IID_ITfLangBarItemSink || !sink) return E_INVALIDARG;
    if (sink_) return E_FAIL;
    HRESULT hr = sink->QueryInterface(IID_PPV_ARGS(&sink_));
    if (FAILED(hr)) return hr;
    Trace(L"ModeLangBarItem AdviseSink succeeded");
    *cookie = 1;
    return S_OK;
}

HRESULT ModeLangBarItem::UnadviseSink(DWORD cookie) {
    if (cookie != 1 || !sink_) return E_FAIL;
    sink_->Release();
    sink_ = nullptr;
    return S_OK;
}

void ModeLangBarItem::RefreshTheme() {
    if (TaskbarIsDark() != darkTaskbar_) NotifyUpdate();
}

void ModeLangBarItem::NotifyUpdate() {
    if (sink_) sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP | TF_LBI_STATUS);
}

InputMode ModeLangBarItem::Mode() const {
    return getMode_ ? getMode_() : InputMode::Convert;
}

bool ModeLangBarItem::JapaneseProfile() const {
    return japaneseProfile_ && japaneseProfile_();
}

}  // namespace tekito::tsf
