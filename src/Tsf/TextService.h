#pragma once

#include "Core/ConversionEngine.h"
#include "Core/InputStateMachine.h"
#include "Core/UserLearning.h"
#include "Tsf/CandidateWindow.h"
#include "Tsf/ComPtr.h"
#include "Tsf/EditSession.h"
#include "Tsf/TekitoGuids.h"
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
                          public ITfCompartmentEventSink,
                          public ITfInputProcessorProfileActivationSink {
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

    // ITfInputProcessorProfileActivationSink: switching between TEKITO's
    // English and Japanese profiles does not always reactivate the service.
    HRESULT STDMETHODCALLTYPE OnActivated(DWORD profileType, LANGID langid, REFCLSID clsid,
                                          REFGUID category, REFGUID profile, HKL layout,
                                          DWORD flags) override;

    HRESULT HandleKeyInEditSession(ITfContext* context, TfEditCookie editCookie,
                                   const KeyInput& input);
    HRESULT HandleKeyInEditSessionCore(ITfContext* context, TfEditCookie editCookie,
                                       const KeyInput& input);

private:
    ~TextService();

    bool TranslateKey(WPARAM wParam, LPARAM lParam, KeyInput& input);
    // The English typing keys, whatever the mode.
    bool TranslateEnglishKey(WPARAM wParam, KeyInput& input);
    // Japanese mode: Shift+letter with nothing typed starts an English word
    // that works like English Auto (TextServiceJapanese.cpp).
    bool TranslateEnglishSegmentKey(WPARAM wParam, KeyInput& input);
    HRESULT HandleEnglishSegmentEnd(ITfContext* context, TfEditCookie editCookie, const KeyInput& input);
    void EndEnglishSegment();
    bool EnsureEngine();
    HRESULT EnsureComposition(ITfContext* context, TfEditCookie editCookie);
    HRESULT ReplaceComposition(ITfContext* context, TfEditCookie editCookie,
                               const std::wstring& text);
    HRESULT ApplyDisplayAttribute(ITfContext* context, TfEditCookie editCookie, ITfRange* range,
                                  REFGUID attribute = GUID_TekitoDisplayAttribute);
    HRESULT EndComposition(TfEditCookie editCookie);
    HRESULT InsertAtSelection(ITfContext* context, TfEditCookie editCookie,
                              const std::wstring& text);
    void RefreshCandidates(ITfContext* context, TfEditCookie editCookie);
    void ShowCandidates(ITfContext* context, TfEditCookie editCookie);
    RECT GetCandidateAnchor(ITfContext* context, TfEditCookie editCookie) const;
    // The caret (or the selection) outside a composition, the composition
    // inside one.
    RECT CaretAnchor(ITfContext* context, TfEditCookie editCookie) const;
    // After the user switched modes: the new mode by the caret, if Settings
    // shows it. `context` may be null (the taskbar button).
    void ShowModeIndicator(ITfContext* context);
    void ShowModeBadge(const RECT& caret);
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
    // Mirrors the mode in the open/close compartment, and in the Japanese
    // profile also in the conversion-mode compartment.
    void SyncModeCompartments();
    void ApplySettings();

    // Japanese profile (TextServiceJapanese.cpp).
    struct ModeKey {
        InputMode mode{InputMode::Direct};
        bool setsInputForm{false};
        japanese::KanaForm inputForm{japanese::KanaForm::Hiragana};
        // Taken without doing anything (Hankaku/Zenkaku when it is not the
        // switch key).
        bool ignored{false};
    };
    // Keys that switch modes: Hankaku/Zenkaku, Henkan and Muhenkan outside
    // a composition, Hiragana/Katakana, IME On/Off.
    bool TranslateModeKey(WPARAM wParam, ModeKey& key) const;
    void ApplyModeKey(ITfContext* context, const ModeKey& key);
    // The mode the switch key (and a click on the mode button) goes to.
    InputMode ToggledMode() const noexcept;
    // Whether Japanese is a mode here: the Japanese profile, with Japanese
    // input turned on in Settings.
    bool JapaneseModeAvailable() const noexcept { return japaneseProfile_ && userSettings_.japaneseEnabled; }
    bool TranslateJapaneseKey(WPARAM wParam, KeyInput& input);
    HRESULT HandleJapaneseKey(ITfContext* context, TfEditCookie editCookie, const KeyInput& input);
    HRESULT ShowJapanesePreedit(ITfContext* context, TfEditCookie editCookie);
    // The composition text as the composer's segments, each underlined by
    // what it is.
    HRESULT ReplaceJapaneseComposition(ITfContext* context, TfEditCookie editCookie);
    void ShowJapaneseCandidates(ITfContext* context, TfEditCookie editCookie);
    // Before new Japanese typing: the composer goes on from what it
    // committed last only when the caret is still right after that text.
    void CheckJapaneseContext(ITfContext* context, TfEditCookie editCookie);
    // What the candidate means, for the pane beside the list; empty when
    // meanings are off or unknown.
    CandidateDetail MeaningFor(std::wstring_view text, std::wstring_view reading = {}) const;
    CandidateDetail SelectedEnglishMeaning() const;
    // Marks the candidates of the page shown that have a meaning (the
    // dictionary sign), when meanings are on.
    void MarkMeanings(std::vector<Candidate>& candidates, std::size_t first, std::size_t count,
                      std::wstring_view reading = {}) const;
    [[nodiscard]] std::vector<Candidate> EnglishRows() const;
    RECT JapaneseCandidateAnchor(ITfContext* context, TfEditCookie editCookie) const;
    HRESULT CommitJapanese(ITfContext* context, TfEditCookie editCookie);
    // The mode shared for the active profile.
    InputMode SharedMode() const noexcept;
    void UpdateActiveProfile();
    // Puts japaneseUserWords_ into the composer's user dictionary.
    void RefreshJapaneseUserWords();
    void SetJapaneseProfile(bool japanese);
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
    // Atoms of the English underline and the three Japanese ones.
    TfGuidAtom displayAttributeAtoms_[4]{TF_INVALID_GUIDATOM, TF_INVALID_GUIDATOM,
                                         TF_INVALID_GUIDATOM, TF_INVALID_GUIDATOM};
    bool processIsElevated_{false};
    bool processPrefersPassThrough_{false};
    bool processExcludedByUser_{false};
    std::wstring processName_;
    DWORD threadManagerSinkCookie_{TF_INVALID_COOKIE};
    DWORD openCloseSinkCookie_{TF_INVALID_COOKIE};
    DWORD conversionSinkCookie_{TF_INVALID_COOKIE};
    DWORD profileSinkCookie_{TF_INVALID_COOKIE};
    ComPtr<ITfContext> sinkContext_;
    DWORD textEditSinkCookie_{TF_INVALID_COOKIE};
    DWORD textLayoutSinkCookie_{TF_INVALID_COOKIE};
    int preservedToggleKey_{0};
    bool updatingCompartments_{false};

    // The mode in effect: Convert or Direct, or Japanese in the ja-JP
    // profile. state_ holds Convert only while English Auto is on.
    InputMode mode_{InputMode::Convert};
    bool japaneseProfile_{false};
    // Windows closes a ja-JP profile right after it activates; until the
    // first key, compartment changes are answered with TEKITO's own mode
    // rather than followed.
    bool activationSettling_{false};
    // An English word typed with Shift in Japanese is being typed; state_
    // runs in Convert meanwhile.
    bool englishSegment_{false};
    japanese::JapaneseComposer japanese_;
    // The text the composer committed last (see CheckJapaneseContext).
    std::wstring lastJapaneseCommit_;
    japanese::JapaneseLearningStore japaneseLearning_;
    // The words the user added for Japanese, as stored and as looked up.
    std::vector<japanese::UserWord> japaneseUserWords_;
    japanese::JapaneseUserDictionary japaneseUserDictionary_;
    // Candidates per page of the Japanese list (UserSettings::candidateRows).
    std::size_t japanesePage_{9};
    RECT candidateAnchor_{};
    CandidateWindow candidateWindow_;
    ComPtr<ITfLangBarItemMgr> langBarItemMgr_;
    ComPtr<ModeLangBarItem> modeLangBarItem_;
};

}  // namespace tekito::tsf
