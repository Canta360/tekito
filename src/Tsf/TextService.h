#pragma once

#include "Core/ConversionEngine.h"
#include "Core/InputStateMachine.h"
#include "Core/UserLearning.h"
#include "Tsf/CandidateWindow.h"
#include "Tsf/ComPtr.h"
#include "Tsf/EditSession.h"
#include "Tsf/ModeLangBarItem.h"
#include "UserData/UserDataRepository.h"
#include "UserData/RuntimeModeState.h"

#include <ctfutb.h>
#include <msctf.h>
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

namespace tekito::tsf {

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfDisplayAttributeInfo,
                          public ITfThreadMgrEventSink,
                          public ITfTextEditSink,
                          public ITfTextLayoutSink,
                          public ITfCompartmentEventSink {
public:
    TextService();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* threadManager, TfClientId clientId) override;
    HRESULT STDMETHODCALLTYPE Deactivate() override;
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* threadManager, TfClientId clientId,
                                         DWORD flags) override;

    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                            BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                        BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                          BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam,
                                      BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID guid,
                                              BOOL* eaten) override;
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie editCookie,
                                                      ITfComposition* composition) override;
    HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) override;
    HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid,
                                                      ITfDisplayAttributeInfo** info) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override;
    HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) override;
    HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE* attribute) override;
    HRESULT STDMETHODCALLTYPE Reset() override;

    // ITfThreadMgrEventSink
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr* documentManager) override;
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* documentManager) override;
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focus,
                                         ITfDocumentMgr* previousFocus) override;
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override;
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override;

    // ITfTextEditSink
    HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context, TfEditCookie readOnlyCookie,
                                        ITfEditRecord* editRecord) override;

    // ITfTextLayoutSink
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* context, TfLayoutCode code,
                                             ITfContextView* view) override;

    // ITfCompartmentEventSink
    HRESULT STDMETHODCALLTYPE OnChange(REFGUID compartment) override;

    HRESULT HandleKeyInEditSession(ITfContext* context, TfEditCookie editCookie,
                                   const KeyInput& input);

private:
    ~TextService();

    bool TranslateKey(WPARAM wParam, LPARAM lParam, KeyInput& input);
    bool EnsureEngine();
    HRESULT EnsureComposition(ITfContext* context, TfEditCookie editCookie);
    HRESULT ReplaceComposition(ITfContext* context, TfEditCookie editCookie,
                               const std::wstring& text);
    HRESULT ApplyDisplayAttribute(ITfContext* context, TfEditCookie editCookie,
                                  ITfRange* range);
    HRESULT EndComposition(TfEditCookie editCookie);
    HRESULT InsertAtSelection(ITfContext* context, TfEditCookie editCookie,
                              const std::wstring& text);
    void RefreshCandidates(ITfContext* context, TfEditCookie editCookie);
    void ShowCandidates(ITfContext* context, TfEditCookie editCookie);
    RECT GetCandidateAnchor(ITfContext* context, TfEditCookie editCookie) const;
    HRESULT RequestKeyEditSession(ITfContext* context, const KeyInput& input);
    void OnCandidateSelected(std::size_t index);
    void SyncRuntimeState();
    void SetInputMode(InputMode mode);
    // Switches the mode for every TEKITO client (shared runtime state).
    void ChangeInputMode(InputMode mode);
    void FinishCompositionLater();
    void AdviseThreadManagerSinks();
    void UnadviseThreadManagerSinks();
    void AdviseContextSinks(ITfContext* context);
    void UnadviseContextSinks();
    void UpdateToggleKey(int key);
    void SyncOpenCloseCompartment();
    void ApplySettings();
    bool ProcessPassesThrough() const noexcept;
    bool LetsKeyThrough(const KeyInput& input) const noexcept;
    void OpenSettings();
    void RegisterLangBarItem();
    void UnregisterLangBarItem();

    std::atomic<ULONG> refCount_{1};
    ITfThreadMgr* threadManager_{nullptr};
    TfClientId clientId_{TF_CLIENTID_NULL};
    ITfComposition* composition_{nullptr};
    ITfContext* compositionContext_{nullptr};
    DWORD activateFlags_{0};

    std::wstring rawText_;
    CapitalizationOrigin capitalizationOrigin_{CapitalizationOrigin::Unknown};
    bool sentenceStart_{false};
    InputStateMachine state_;
    UserDictionary userDictionary_;
    UserLearningStore userLearning_;
    UserLearningProvider userLearningProvider_;
    SocialLearningStore socialLearning_;
    SocialLearningProvider socialLearningProvider_;
    std::unique_ptr<IConversionEngine> candidateEngine_;
    std::unique_ptr<userdata::IUserDataRepository> settingsRepository_;
    std::unique_ptr<userdata::RuntimeModeState> runtimeMode_;
    userdata::UserSettings userSettings_{};
    bool settingsLoaded_{false};
    std::uint32_t runtimeModeGeneration_{0};
    std::uint32_t runtimeSettingsGeneration_{0};
    std::uint32_t runtimeDictionaryGeneration_{0};
    std::uint32_t runtimeLearningGeneration_{0};
    TfGuidAtom displayAttributeAtom_{TF_INVALID_GUIDATOM};
    bool processIsElevated_{false};
    bool processPrefersPassThrough_{false};
    bool processExcludedByUser_{false};
    std::wstring processName_;
    DWORD threadManagerSinkCookie_{TF_INVALID_COOKIE};
    DWORD openCloseSinkCookie_{TF_INVALID_COOKIE};
    ComPtr<ITfContext> sinkContext_;
    DWORD textEditSinkCookie_{TF_INVALID_COOKIE};
    DWORD textLayoutSinkCookie_{TF_INVALID_COOKIE};
    int preservedToggleKey_{0};
    bool updatingOpenClose_{false};
    RECT candidateAnchor_{};
    CandidateWindow candidateWindow_;
    ComPtr<ITfLangBarItemMgr> langBarItemMgr_;
    ComPtr<ModeLangBarItem> modeLangBarItem_;
};

}  // namespace tekito::tsf
