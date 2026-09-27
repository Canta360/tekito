#pragma once

#include "Core/InputMode.h"

#include <ctfutb.h>
#include <windows.h>
#include <atomic>
#include <functional>

namespace tekito::tsf {

class ModeLangBarItem final : public ITfLangBarItemButton,
                              public ITfSource {
public:
    using ModeGetter = std::function<InputMode()>;
    using ModeSetter = std::function<void(InputMode)>;
    using SettingsLauncher = std::function<void()>;
    using LanguageQuery = std::function<bool()>;  // true when the UI is in Japanese

    ModeLangBarItem(HINSTANCE instance, ModeGetter getMode, ModeSetter setMode,
                    SettingsLauncher launchSettings, LanguageQuery japanese);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO* info) override;
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD* status) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR* tooltip) override;
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT pt, const RECT* area) override;
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu* menu) override;
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT id) override;
    HRESULT STDMETHODCALLTYPE GetIcon(HICON* icon) override;
    HRESULT STDMETHODCALLTYPE GetText(BSTR* text) override;
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) override;
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override;

    void NotifyUpdate();

private:
    ~ModeLangBarItem();

    [[nodiscard]] InputMode Mode() const;
    [[nodiscard]] const wchar_t* Text(const wchar_t* english, const wchar_t* japanese) const;
    void ShowContextMenu(POINT point);

    std::atomic<ULONG> refCount_{1};
    HINSTANCE instance_{nullptr};
    ModeGetter getMode_;
    ModeSetter setMode_;
    SettingsLauncher launchSettings_;
    LanguageQuery japanese_;
    ITfLangBarItemSink* sink_{nullptr};
};

}  // namespace tekito::tsf
