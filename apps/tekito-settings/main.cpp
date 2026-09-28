#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Core/ExternalLexiconProvider.h"
#include "Dictionary/ExternalDictionaryProvider.h"
#include "UserData/DataPackValidation.h"
#include "UserData/KeyboardLayout.h"
#include "UserData/RuntimeModeState.h"
#include "UserData/UiLanguage.h"
#include "UserData/UserDataRepository.h"
#include "UserData/UserDictionaryFile.h"
#include "assets/tekito_resource.h"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>
#include "WebView2.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kClassName[] = L"TekitoSettingsWindow";

// From project(VERSION) in CMakeLists.txt.
#define TEKITO_WIDEN_(text) L##text
#define TEKITO_WIDEN(text) TEKITO_WIDEN_(text)
constexpr wchar_t kVersion[] = TEKITO_WIDEN(TEKITO_VERSION_STRING);
constexpr int kRetryWebView = 200;
// How often the host checks the shared runtime state for changes made
// outside this window (mode switched from the Language Bar, the IME saving
// learning data, ...). Reading four counters is essentially free; the page
// is only sent a new state when one of them moved.
constexpr UINT_PTR kStateWatchTimer = 1;
constexpr UINT kStateWatchIntervalMs = 500;

// The distributor's site, opened by the About page. Fixed here rather than
// taken from the page, so the page cannot ask the host to open arbitrary URLs.
constexpr wchar_t kPublisherWebsite[] = L"https://capitata.dev";

// The Data Packs a full installation has, in the order About lists them.
struct DataPackInfo {
    const wchar_t* folder;
    const wchar_t* label;
};
// The Japanese ones, installed only with Japanese input. The first two are
// what Japanese needs at all; the rest add to it.
constexpr DataPackInfo kJapaneseDataPacks[] = {
    {L"japanese-core", L"Japanese Dictionary"},
    {L"japanese-romaji", L"Romaji Table"},
    {L"japanese-lm", L"Japanese Word Pairs"},
    {L"japanese-loanwords", L"Loanwords"},
    {L"japanese-wiktionary", L"Japanese Meanings (Wiktionary)"},
    {L"japanese-wordnet", L"Japanese Meanings (WordNet)"},
};
constexpr std::size_t kJapaneseRequiredPacks = 2;
constexpr DataPackInfo kDataPacks[] = {
    {L"standard-english", L"Standard English"},
    {L"wikipedia-common-misspellings", L"Common Misspellings"},
    {L"frequency", L"Frequency"},
    {L"phrase", L"Phrase / N-gram"},
    {L"dictionary-display", L"Dictionary Display"},
    {L"proper-nouns", L"Place Names"},
    {L"slang", L"Slang"},
    {L"wiktionary-slang", L"Extended Slang"},
    {L"pronunciation", L"Pronunciation"},
    {L"emoji", L"Emoji"},
    {L"social-expression", L"Social Expressions"},
    {L"japanese-phonetic", L"Japanese Phonetic Suggestions"},
    {L"qwerty-typo-catalog", L"QWERTY Typo Evaluation"},
};

// Windows accent color as "#rrggbb" (the DWM value is stored as 0xAABBGGRR).
std::wstring AccentColorHex() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return L"#0078d4";
    }
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"#%02x%02x%02x", value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff);
    return buffer;
}

bool AppsUseDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                        &size) == ERROR_SUCCESS &&
           value == 0;
}

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

std::wstring JsonEscape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 8);
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (character < 0x20) {
                constexpr wchar_t digits[] = L"0123456789abcdef";
                result += L"\\u000";
                result += digits[character & 0x0f];
            } else {
                result += character;
            }
            break;
        }
    }
    return result;
}

// A request from the Settings page: one flat JSON object whose values are
// strings, numbers or booleans (see ui/src/host.js). Anything nested is
// skipped; a malformed message reads as empty.
class WebMessage final {
public:
    static WebMessage Parse(std::wstring_view json) {
        WebMessage message;
        Reader reader{json};
        if (!reader.Consume(L'{')) return {};
        if (reader.Consume(L'}')) return message;
        do {
            std::wstring key;
            if (!reader.ReadString(key) || !reader.Consume(L':')) return {};
            Value value;
            if (!reader.ReadValue(value)) return {};
            message.values_[std::move(key)] = std::move(value);
        } while (reader.Consume(L','));
        return reader.Consume(L'}') ? message : WebMessage{};
    }

    std::wstring String(std::wstring_view key) const {
        const auto* value = Find(key);
        return value && value->kind == Value::Kind::String ? value->text : std::wstring{};
    }

    bool Bool(std::wstring_view key, bool fallback = false) const {
        const auto* value = Find(key);
        if (!value) return fallback;
        if (value->kind == Value::Kind::Bool) return value->boolean;
        if (value->kind == Value::Kind::Number) return value->number != 0.0;
        return fallback;
    }

    // Numbers, with booleans read as 0 and 1 ("value" carries either).
    std::int64_t Number(std::wstring_view key, std::int64_t fallback = 0) const {
        const auto* value = Find(key);
        if (!value) return fallback;
        if (value->kind == Value::Kind::Number) return static_cast<std::int64_t>(value->number);
        if (value->kind == Value::Kind::Bool) return value->boolean ? 1 : 0;
        return fallback;
    }

private:
    struct Value {
        enum class Kind { Null, String, Number, Bool } kind{Kind::Null};
        std::wstring text;
        double number{0.0};
        bool boolean{false};
    };

    struct Reader {
        std::wstring_view json;
        std::size_t index{0};

        void SkipSpace() {
            while (index < json.size() && std::iswspace(json[index])) ++index;
        }

        bool Consume(wchar_t expected) {
            SkipSpace();
            if (index >= json.size() || json[index] != expected) return false;
            ++index;
            return true;
        }

        bool ConsumeWord(std::wstring_view word) {
            if (json.substr(index, word.size()) != word) return false;
            index += word.size();
            return true;
        }

        bool ReadString(std::wstring& out) {
            if (!Consume(L'"')) return false;
            while (index < json.size()) {
                const wchar_t ch = json[index++];
                if (ch == L'"') return true;
                if (ch != L'\\') {
                    out += ch;
                    continue;
                }
                if (index >= json.size()) return false;
                switch (const wchar_t escaped = json[index++]) {
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'u': {
                    if (index + 4 > json.size()) return false;
                    unsigned code = 0;
                    for (int digit = 0; digit < 4; ++digit) {
                        const wchar_t hex = json[index++];
                        code <<= 4;
                        if (hex >= L'0' && hex <= L'9') code |= hex - L'0';
                        else if (hex >= L'a' && hex <= L'f') code |= hex - L'a' + 10;
                        else if (hex >= L'A' && hex <= L'F') code |= hex - L'A' + 10;
                        else return false;
                    }
                    out += static_cast<wchar_t>(code);
                    break;
                }
                default: out += escaped; break;  // \" \\ \/
                }
            }
            return false;
        }

        bool ReadValue(Value& value) {
            SkipSpace();
            if (index >= json.size()) return false;
            const wchar_t ch = json[index];
            if (ch == L'"') {
                value.kind = Value::Kind::String;
                return ReadString(value.text);
            }
            if (ConsumeWord(L"true")) {
                value.kind = Value::Kind::Bool;
                value.boolean = true;
                return true;
            }
            if (ConsumeWord(L"false")) {
                value.kind = Value::Kind::Bool;
                return true;
            }
            if (ConsumeWord(L"null")) return true;
            if (ch == L'{' || ch == L'[') return SkipNested();
            const auto begin = index;
            while (index < json.size() && (std::iswdigit(json[index]) || json[index] == L'-' ||
                                           json[index] == L'+' || json[index] == L'.' ||
                                           json[index] == L'e' || json[index] == L'E')) {
                ++index;
            }
            if (index == begin) return false;
            value.kind = Value::Kind::Number;
            value.number = std::wcstod(std::wstring(json.substr(begin, index - begin)).c_str(),
                                       nullptr);
            return true;
        }

        bool SkipNested() {
            int depth = 0;
            do {
                if (index >= json.size()) return false;
                const wchar_t ch = json[index];
                if (ch == L'"') {
                    std::wstring ignored;
                    if (!ReadString(ignored)) return false;
                    continue;
                }
                if (ch == L'{' || ch == L'[') ++depth;
                if (ch == L'}' || ch == L']') --depth;
                ++index;
            } while (depth > 0);
            return true;
        }
    };

    const Value* Find(std::wstring_view key) const {
        const auto found = values_.find(key);
        return found == values_.end() ? nullptr : &found->second;
    }

    std::map<std::wstring, Value, std::less<>> values_;
};

std::wstring RegisteredTsfDll() {
    constexpr wchar_t keyPath[] =
        L"CLSID\\{6F67E5C8-A873-4B69-8EC3-26DF00F642F1}\\InprocServer32";
    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CLASSES_ROOT, keyPath, nullptr, RRF_RT_REG_SZ, nullptr,
                        value, &size) == ERROR_SUCCESS
               ? std::wstring(value)
               : std::wstring{};
}

std::filesystem::path LogFolder() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, path);
    return length == 0 || length >= MAX_PATH ? std::filesystem::path{}
                                             : std::filesystem::path(path);
}

std::wstring PolicyName(std::uint32_t flags) {
    if (flags & tekito::CandidatePolicyProtect) return L"Protect original";
    if (flags & tekito::CandidatePolicyCorrect) return L"Correct spelling";
    if (flags & tekito::CandidatePolicyNormalize) return L"Normalize";
    if (flags & tekito::CandidatePolicyExpand) return L"Expand abbreviation";
    if (flags & tekito::CandidatePolicyContext) return L"Context only";
    return L"Suggest alternative";
}

bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool copied = false;
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            CopyMemory(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            if (!copied) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return length == 0 || length == MAX_PATH
               ? std::filesystem::path{}
               : std::filesystem::path(path, path + length).parent_path();
}

std::filesystem::path WebViewUserDataDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(localAppData) / L"TEKITO" / L"settings-webview";
}

class SettingsApp final {
public:
    bool Initialize(HINSTANCE instance) {
        instance_ = instance;
        repository_ = tekito::userdata::CreateDefaultUserDataRepository();
        if (!repository_ || !repository_->Open()) return false;
        (void)repository_->Load(dictionary_);
        (void)repository_->LoadJapaneseUserWords(japaneseWords_);
        (void)repository_->LoadSettings(settings_);
        (void)repository_->LoadLearning(learning_);
        const auto startupMode = settings_.restoreLastInputMode
                                     ? settings_.lastInputMode
                                     : settings_.defaultInputMode;
        runtime_ = std::make_unique<tekito::userdata::RuntimeModeState>(
            startupMode, tekito::userdata::kRuntimeStateName, settings_.lastJapaneseProfileMode);

        WNDCLASSW windowClass{};
        windowClass.hInstance = instance_;
        windowClass.lpfnWndProc = &SettingsApp::WindowProc;
        windowClass.lpszClassName = kClassName;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TEKITO));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        hwnd_ = CreateWindowExW(0, kClassName, WindowTitle(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                    WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME |
                                    WS_VISIBLE | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1040, 720,
                                nullptr, nullptr, instance_, this);
        if (hwnd_) {
            const auto icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_TEKITO));
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
        return hwnd_ != nullptr;
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    void CreateWebViewStatusControls() {
        fallbackLabel_ = CreateWindowExW(
            0, L"STATIC", Text(L"Loading TEKITO Settings...", L"TEKITO の設定を読み込んでいます…"),
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            32, 32, 876, 64, hwnd_, nullptr, instance_, nullptr);
        fallbackRetry_ = CreateWindowExW(
            0, L"BUTTON", Text(L"Retry", L"再試行"),
            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            410, 112, 120, 32, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRetryWebView)),
            instance_, nullptr);
        if (fallbackLabel_) {
            SendMessageW(fallbackLabel_, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        }
        if (fallbackRetry_) {
            SendMessageW(fallbackRetry_, WM_SETFONT,
                         reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            ShowWindow(fallbackRetry_, SW_HIDE);
        }
    }

    void SetWebViewStatus(std::wstring_view message, bool showRetry) {
        if (fallbackLabel_) {
            SetWindowTextW(fallbackLabel_, std::wstring(message).c_str());
            ShowWindow(fallbackLabel_, SW_SHOW);
        }
        if (fallbackRetry_) ShowWindow(fallbackRetry_, showRetry ? SW_SHOW : SW_HIDE);
        if (controller_) controller_->put_IsVisible(showRetry ? FALSE : TRUE);
    }

    void FailWebView(std::wstring_view reason) {
        std::wstring message = Text(L"TEKITO Settings could not be loaded.\r\n",
                                    L"TEKITO の設定を開けませんでした。\r\n");
        message += reason;
        SetWebViewStatus(message, true);
    }

    void ResetWebView() {
        if (webview_) {
            webview_->remove_WebMessageReceived(webMessageToken_);
            webview_->remove_NavigationCompleted(navigationToken_);
        }
        if (controller_) controller_->Close();
        webview_.Reset();
        webview3_.Reset();
        controller_.Reset();
        webMessageToken_ = {};
        navigationToken_ = {};
    }

    bool ValidateSettingsUi(std::wstring& error) const {
        const auto root = ExecutableDirectory() / L"settings-ui";
        if (!std::filesystem::exists(root / L"index.html")) {
            error = Text(L"The local settings package is missing index.html.", L"設定画面のファイル（index.html）が見つかりません。");
            return false;
        }
        for (const auto* icon : {L"auto.ico", L"direct.ico", L"direct-dark.ico", L"japanese.ico",
                                 L"japanese-dark.ico", L"tekito.ico"}) {
            const auto path = root / L"assets" / icon;
            std::ifstream file(path, std::ios::binary);
            unsigned char header[4]{};
            if (!std::filesystem::is_regular_file(path) ||
                !file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
                header[0] != 0 || header[1] != 0 || header[2] != 1 || header[3] != 0) {
                error = Text(L"The installed settings package is missing a provided ICO asset.", L"設定画面のアイコンファイルが見つかりません。");
                return false;
            }
        }
        return true;
    }

    void InitializeWebView() {
        ResetWebView();
        SetWebViewStatus(Text(L"Loading TEKITO Settings...", L"TEKITO の設定を読み込んでいます…"), false);
        std::wstring uiError;
        if (!ValidateSettingsUi(uiError)) {
            FailWebView(uiError);
            return;
        }
        const auto userData = WebViewUserDataDirectory();
        if (userData.empty()) {
            FailWebView(Text(L"The WebView2 user data folder could not be resolved.", L"WebView2 のデータフォルダーの場所を特定できませんでした。"));
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(userData, error);
        if (error) {
            FailWebView(Text(L"The WebView2 user data folder could not be created.", L"WebView2 のデータフォルダーを作成できませんでした。"));
            return;
        }
        const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, userData.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT environmentResult, ICoreWebView2Environment* environment) -> HRESULT {
                    if (FAILED(environmentResult) || !environment) {
                        FailWebView(Text(L"WebView2 Runtime is unavailable or could not start.", L"WebView2 Runtime が見つからないか、起動できませんでした。"));
                        return S_OK;
                    }
                    return environment->CreateCoreWebView2Controller(
                        hwnd_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT controllerResult,
                                   ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(controllerResult) || !controller) {
                                    FailWebView(Text(L"WebView2 could not create the settings view.", L"WebView2 で設定画面を作成できませんでした。"));
                                    return S_OK;
                                }
                                controller_ = controller;
                                ApplyTheme();
                                if (FAILED(controller_->get_CoreWebView2(&webview_)) || !webview_) {
                                    FailWebView(Text(L"WebView2 returned no settings view.", L"WebView2 から設定画面が返されませんでした。"));
                                    return S_OK;
                                }
                                const auto uiFolder = (ExecutableDirectory() / L"settings-ui").wstring();
                                if (FAILED(webview_.As(&webview3_)) ||
                                    FAILED(webview3_->SetVirtualHostNameToFolderMapping(
                                        L"tekito.settings", uiFolder.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW))) {
                                    FailWebView(Text(L"The local settings package could not be mapped.", L"設定画面のファイルを読み込めませんでした。"));
                                    return S_OK;
                                }
                                webview_->add_NavigationCompleted(
                                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                            BOOL success = FALSE;
                                            if (args) args->get_IsSuccess(&success);
                                            if (!success) {
                                                FailWebView(Text(L"The local settings page failed to navigate.", L"設定画面を表示できませんでした。"));
                                            } else {
                                                if (fallbackLabel_) ShowWindow(fallbackLabel_, SW_HIDE);
                                                if (fallbackRetry_) ShowWindow(fallbackRetry_, SW_HIDE);
                                                ResizeWebView();
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &navigationToken_);
                                webview_->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*,
                                               ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            LPWSTR message = nullptr;
                                            if (args && SUCCEEDED(args->TryGetWebMessageAsString(&message)) &&
                                                message) {
                                                HandleWebMessage(message);
                                                CoTaskMemFree(message);
                                            }
                                            return S_OK;
                                        }).Get(),
                                    &webMessageToken_);
                                RECT bounds{};
                                GetClientRect(hwnd_, &bounds);
                                controller_->put_Bounds(bounds);
                                controller_->put_IsVisible(TRUE);
                                const HRESULT navigationResult =
                                    webview_->Navigate(L"https://tekito.settings/index.html");
                                if (FAILED(navigationResult)) {
                                    FailWebView(Text(L"The local settings page could not be opened.", L"設定画面を開けませんでした。"));
                                }
                                return S_OK;
                            }).Get());
                }).Get());
        if (FAILED(result)) {
            FailWebView(Text(L"WebView2 environment creation failed.", L"WebView2 の準備に失敗しました。"));
        }
    }

    // Title bar and the WebView's pre-paint background follow the Windows
    // app theme, so a dark page never flashes white while it loads.
    void ApplyTheme() {
        const BOOL dark = AppsUseDarkTheme() ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        ComPtr<ICoreWebView2Controller2> controller2;
        if (controller_ && SUCCEEDED(controller_.As(&controller2))) {
            const COREWEBVIEW2_COLOR background =
                dark ? COREWEBVIEW2_COLOR{255, 10, 16, 20} : COREWEBVIEW2_COLOR{255, 238, 243, 246};
            controller2->put_DefaultBackgroundColor(background);
        }
    }

    struct Generations {
        std::uint32_t mode{0};
        std::uint32_t settings{0};
        std::uint32_t dictionary{0};
        std::uint32_t learning{0};
        bool operator==(const Generations&) const = default;
    };

    Generations CurrentGenerations() const {
        if (!runtime_) return {};
        return {runtime_->ModeGeneration(), runtime_->SettingsGeneration(),
                runtime_->DictionaryGeneration(), runtime_->LearningGeneration()};
    }

    // Picks up changes made by other processes and pushes the new state to
    // the page. Changes this window made itself are already reflected in its
    // reply, so the generations are re-read after every handled request.
    void CheckForExternalChanges() {
        if (!runtime_ || !repository_ || !webview_) return;
        const auto now = CurrentGenerations();
        if (now == seenGenerations_) return;
        if (now.settings != seenGenerations_.settings) {
            (void)repository_->LoadSettings(settings_);
        }
        if (now.dictionary != seenGenerations_.dictionary) {
            tekito::UserDictionary reloaded;
            if (repository_->Load(reloaded)) dictionary_ = std::move(reloaded);
        }
        if (now.learning != seenGenerations_.learning) {
            tekito::UserLearningStore reloaded;
            if (repository_->LoadLearning(reloaded)) learning_ = std::move(reloaded);
        }
        seenGenerations_ = now;
        PostState();
    }

    void PostState() {
        if (!webview_) return;
        const auto message = L"{\"type\":\"state.changed\",\"state\":" + SettingsStateJson() + L"}";
        webview_->PostWebMessageAsString(message.c_str());
    }

    void ResizeWebView() {
        RECT bounds{};
        GetClientRect(hwnd_, &bounds);
        if (controller_) controller_->put_Bounds(bounds);
        if (fallbackLabel_) {
            const int width = std::max(280L, bounds.right - bounds.left - 64);
            SetWindowPos(fallbackLabel_, nullptr, 32, 32, width, 64,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            if (fallbackRetry_) {
                SetWindowPos(fallbackRetry_, nullptr, (width - 120) / 2 + 32, 112,
                             120, 32, SWP_NOACTIVATE | SWP_NOZORDER);
            }
        }
    }

    std::wstring ResponseJson(bool ok, std::wstring_view error = {}) {
        return L"{\"ok\":" + std::wstring(ok ? L"true" : L"false") +
               L",\"error\":\"" + JsonEscape(error) +
               L"\",\"state\":" + SettingsStateJson() + L"}";
    }

    // `extra` is additional JSON members for the reply, e.g. L",\"text\":\"...\"".
    void Reply(std::wstring_view requestId, bool ok, std::wstring_view error = {},
               std::wstring_view extra = {}) {
        if (!webview_ || requestId.empty()) return;
        auto response = ResponseJson(ok, error);
        response.insert(response.size() - 1, extra);
        const auto message = L"{\"requestId\":\"" + JsonEscape(requestId) + L"\"," + response.substr(1);
        webview_->PostWebMessageAsString(message.c_str());
    }

    // The TEKITO license agreement, embedded in this executable.
    std::wstring LicenseText() const {
        const HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_LICENSE_TEXT), RT_RCDATA);
        const HGLOBAL loaded = resource ? LoadResource(instance_, resource) : nullptr;
        const DWORD size = resource ? SizeofResource(instance_, resource) : 0;
        const auto* bytes = loaded && size ? static_cast<const char*>(LockResource(loaded)) : nullptr;
        if (!bytes) return {};
        const int count = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), nullptr, 0);
        std::wstring text(static_cast<std::size_t>(std::max(count, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(size), text.data(), count);
        return text;
    }

    std::wstring DataPacksJson() {
        if (!dataPacksJson_.empty()) return dataPacksJson_;
        const auto root = tekito::ExternalLexiconProvider::DataPackRoot();
        std::wstring json = L"[";
        const auto add = [&](const DataPackInfo& pack, const wchar_t* group, bool& valid) {
            tekito::userdata::DataPackStatus status;
            valid = tekito::userdata::ValidateDataPack(root / pack.folder, status);
            if (json.size() > 1) json += L',';
            const auto path = status.packPath.empty() ? root / pack.folder : status.packPath;
            const auto name = status.displayName.empty() ? std::wstring(pack.label) : status.displayName;
            json += L"{\"displayName\":\"" + JsonEscape(name) + L"\",\"group\":\"" + group +
                    L"\",\"valid\":" + std::wstring(valid ? L"true" : L"false") + L",\"version\":\"" +
                    JsonEscape(status.version) + L"\",\"packPath\":\"" + JsonEscape(path.wstring()) +
                    L"\",\"reason\":\"" + JsonEscape(status.reason) + L"\",\"source\":\"" +
                    JsonEscape(status.source) + L"\",\"license\":\"" + JsonEscape(status.license) +
                    L"\",\"noticePath\":\"" + JsonEscape(status.noticePath.wstring()) + L"\"}";
        };
        std::size_t validCount = 0;
        for (const auto& pack : kDataPacks) {
            bool valid = false;
            add(pack, L"english", valid);
            if (valid) ++validCount;
        }
        // Japanese is on only with its required packs; missing optional
        // ones leave it on with less.
        std::size_t japaneseCount = 0;
        japaneseAvailable_ = true;
        for (std::size_t i = 0; i < std::size(kJapaneseDataPacks); ++i) {
            bool valid = false;
            add(kJapaneseDataPacks[i], L"japanese", valid);
            if (valid) ++japaneseCount;
            if (!valid && i < kJapaneseRequiredPacks) japaneseAvailable_ = false;
        }
        dataPackSummary_ = std::to_wstring(validCount) + L" / " + std::to_wstring(std::size(kDataPacks));
        japaneseDataSummary_ =
            std::to_wstring(japaneseCount) + L" / " + std::to_wstring(std::size(kJapaneseDataPacks));
        dataPacksJson_ = json + L']';
        return dataPacksJson_;
    }

    // The mode Settings shows and sets: the Japanese profile's when Japanese
    // is installed, otherwise the English profile's.
    const wchar_t* CurrentModeName() {
        (void)DataPacksJson();
        if (!runtime_) return L"auto";
        if (japaneseAvailable_) {
            const auto mode = runtime_->JapaneseMode();
            return mode == tekito::InputMode::Japanese ? (settings_.japaneseEnabled ? L"japanese" : L"auto")
                   : mode == tekito::InputMode::Direct ? L"direct"
                                                       : L"auto";
        }
        return runtime_->Mode() == tekito::InputMode::Direct ? L"direct" : L"auto";
    }

    std::wstring DictionaryJson() const {
        std::wstring json = L"[";
        for (const auto& entry : dictionary_.Entries()) {
            if (json.size() > 1) json += L',';
            json += L"{\"id\":" + std::to_wstring(entry.id) + L",\"raw\":\"" +
                    JsonEscape(entry.raw) + L"\",\"candidate\":\"" + JsonEscape(entry.candidate) +
                    L"\",\"type\":\"Word\",\"policy\":\"" +
                    JsonEscape(PolicyName(entry.policyFlags)) + L"\"}";
        }
        return json + L']';
    }

    std::wstring JapaneseWordsJson() const {
        std::wstring json = L"[";
        for (const auto& word : japaneseWords_) {
            if (json.size() > 1) json += L',';
            json += L"{\"reading\":\"" + JsonEscape(word.reading) + L"\",\"surface\":\"" + JsonEscape(word.surface) +
                    L"\",\"kind\":\"" + JsonEscape(tekito::japanese::KindName(word.kind)) + L"\"}";
        }
        return json + L']';
    }

    // A reading in hiragana: katakana becomes hiragana; anything else is
    // not a reading.
    static std::optional<std::wstring> HiraganaReading(std::wstring_view text) {
        std::wstring reading;
        for (wchar_t c : text) {
            if (c >= 0x30A1 && c <= 0x30F6) c = static_cast<wchar_t>(c - 0x60);
            const bool kana = (c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x309D || c == 0x309E;
            if (!kana) return std::nullopt;
            reading.push_back(c);
        }
        if (reading.empty()) return std::nullopt;
        return reading;
    }

    // Adds a word, or replaces the one named by originalReading and
    // originalSurface.
    bool SaveJapaneseWordFromMessage(const WebMessage& message, std::wstring& error) {
        const auto reading = HiraganaReading(message.String(L"reading"));
        const auto surface = message.String(L"surface");
        const auto kind = tekito::japanese::KindFromName(message.String(L"kind"));
        if (!reading) {
            error = Text(L"Enter the reading in hiragana.", L"読みはひらがなで入れてください。");
            return false;
        }
        if (surface.empty() || !kind) {
            error = Text(L"Enter the word.", L"単語を入れてください。");
            return false;
        }
        const auto originalReading = message.String(L"originalReading");
        const auto originalSurface = message.String(L"originalSurface");
        auto words = japaneseWords_;
        std::erase_if(words, [&](const tekito::japanese::UserWord& word) {
            return (word.reading == originalReading && word.surface == originalSurface) ||
                   (word.reading == *reading && word.surface == surface);
        });
        words.push_back({*reading, surface, *kind});
        if (!repository_->SaveJapaneseUserWords(words)) {
            error = Text(L"The word could not be saved.", L"単語を保存できませんでした。");
            return false;
        }
        japaneseWords_ = std::move(words);
        runtime_->NotifyDictionaryChanged();
        return true;
    }

    std::wstring SettingsStateJson() {
        const auto mode = CurrentModeName();
        const auto registeredDll = RegisteredTsfDll();
        const auto tsf = !registeredDll.empty() && std::filesystem::exists(registeredDll) ? L"Loaded" : L"Unavailable";
        const auto packs = DataPacksJson();
        std::wstring json = L"{\"version\":\"" + std::wstring(kVersion) + L"\",\"settings\":{\"restoreLastInputMode\":";
        json += settings_.restoreLastInputMode ? L"true" : L"false";
        json += L",\"correctionEnabled\":";
        json += settings_.correctionEnabled ? L"true" : L"false";
        json += L",\"commonMisspellingsEnabled\":";
        json += settings_.commonMisspellingsEnabled ? L"true" : L"false";
        json += L",\"contextSuggestionsEnabled\":";
        json += settings_.contextSuggestionsEnabled ? L"true" : L"false";
        json += L",\"completionEnabled\":";
        json += settings_.completionEnabled ? L"true" : L"false";
        json += L",\"candidateWindowEnabled\":";
        json += settings_.candidateWindowEnabled ? L"true" : L"false";
        json += L",\"learningEnabled\":";
        json += settings_.learningEnabled ? L"true" : L"false";
        json += L",\"japanesePhoneticSuggestionsEnabled\":";
        json += settings_.japanesePhoneticSuggestionsEnabled ? L"true" : L"false";
        json += L",\"socialExpressionRange\":" + std::to_wstring(settings_.socialExpressionRange);
        json += L",\"socialPersonalization\":" + std::to_wstring(settings_.socialPersonalization);
        json += L",\"candidateWindowStyle\":" + std::to_wstring(settings_.candidateWindowStyle);
        json += L",\"candidateRows\":" + std::to_wstring(settings_.candidateRows);
        json += L",\"japaneseSwitchOrder\":" + std::to_wstring(settings_.japaneseSwitchOrder);
        json += L",\"modeIndicatorEnabled\":";
        json += settings_.modeIndicatorEnabled ? L"true" : L"false";
        json += L",\"japaneseEnabled\":";
        json += settings_.japaneseEnabled ? L"true" : L"false";
        json += L",\"meaningsEnabled\":";
        json += settings_.meaningsEnabled ? L"true" : L"false";
        json += L",\"toggleKey\":" + std::to_wstring(settings_.toggleKey);
        json += L",\"keyboardType\":" + std::to_wstring(settings_.keyboardType);
        json += L",\"periodOnEnter\":";
        json += settings_.periodOnEnter ? L"true" : L"false";
        json += L",\"japaneseSpaceWidth\":" + std::to_wstring(settings_.japaneseSpaceWidth);
        json += L",\"japanesePunctuation\":" + std::to_wstring(settings_.japanesePunctuation);
        json += L",\"japanesePredictionEnabled\":";
        json += settings_.japanesePredictionEnabled ? L"true" : L"false";
        json += L",\"uiLanguage\":" + std::to_wstring(settings_.uiLanguage);
        json += L",\"excludedApps\":" + NameListJson(settings_.excludedApps);
        json += L",\"builtInExcludedApps\":" +
                NameListJson({std::begin(tekito::userdata::kBuiltInExcludedApps),
                              std::end(tekito::userdata::kBuiltInExcludedApps)});
        json += L"},\"mode\":\"" + std::wstring(mode) + L"\",\"runtime\":{\"tsf\":\"" +
                tsf + L"\",\"dataPacks\":\"" + dataPackSummary_ + L"\",\"japaneseData\":\"" +
                japaneseDataSummary_ + L"\",\"japanese\":" + (japaneseAvailable_ ? L"true" : L"false") +
                L",\"japaneseKeyboard\":" +
                (tekito::userdata::UsesJapaneseKeyboard(settings_.keyboardType) ? L"true" : L"false") + L"},\"learning\":{\"enabled\":";
        json += settings_.learningEnabled ? L"true" : L"false";
        json += L",\"count\":" + std::to_wstring(learning_.Entries().size() + learning_.PreferenceCount()) + L"},\"dictionary\":";
        json += DictionaryJson();
        json += L",\"japaneseWords\":" + JapaneseWordsJson();
        json += L",\"packs\":" + packs;
        json += L",\"appearance\":{\"accent\":\"" + AccentColorHex() + L"\",\"systemLanguage\":\"" +
                std::wstring(tekito::userdata::UseJapaneseUi(0) ? L"ja" : L"en") + L"\"}}";
        return json;
    }

    static std::wstring NameListJson(const std::vector<std::wstring>& names) {
        std::wstring json = L"[";
        for (const auto& name : names) {
            if (json.size() > 1) json += L',';
            json += L'"' + JsonEscape(name) + L'"';
        }
        return json + L']';
    }

    // Accepts "code", "Code.exe" or a full path; stores the file name.
    static std::wstring ExecutableName(std::wstring_view input) {
        auto name = std::filesystem::path(std::wstring(input)).filename().wstring();
        while (!name.empty() && std::iswspace(name.back())) name.pop_back();
        while (!name.empty() && std::iswspace(name.front())) name.erase(name.begin());
        if (name.empty()) return {};
        if (std::filesystem::path(name).extension().empty()) name += L".exe";
        return name;
    }

    bool AddExcludedApp(std::wstring_view input, std::wstring& error) {
        const auto name = ExecutableName(input);
        if (name.empty()) {
            error = Text(L"Enter an app name, such as code.exe.", L"アプリ名を入力してください（例: code.exe）。");
            return false;
        }
        const auto sameName = [&](const std::wstring& other) {
            return _wcsicmp(other.c_str(), name.c_str()) == 0;
        };
        const auto& builtIn = tekito::userdata::kBuiltInExcludedApps;
        if (std::any_of(settings_.excludedApps.begin(), settings_.excludedApps.end(), sameName) ||
            std::any_of(std::begin(builtIn), std::end(builtIn),
                        [&](const wchar_t* app) { return sameName(app); })) {
            return true;
        }
        const auto previous = settings_;
        settings_.excludedApps.push_back(name);
        if (!SaveSettings()) {
            settings_ = previous;
            error = Text(L"The app list could not be saved.", L"アプリの一覧を保存できませんでした。");
            return false;
        }
        return true;
    }

    std::uint32_t PolicyFlagsFromName(std::wstring_view name) const {
        if (name == L"Protect original") return tekito::CandidatePolicyProtect;
        if (name == L"Correct spelling") return tekito::CandidatePolicyCorrect;
        if (name == L"Normalize") return tekito::CandidatePolicyNormalize;
        if (name == L"Expand abbreviation") return tekito::CandidatePolicyExpand;
        if (name == L"Context only") return tekito::CandidatePolicyContext;
        return tekito::CandidatePolicySuggestOnly;
    }

    bool SaveDictionaryFromMessage(const WebMessage& message, bool update,
                                   std::wstring& error) {
        const auto raw = message.String(L"raw");
        const auto candidate = message.String(L"candidate");
        if (raw.empty() || candidate.empty()) {
            error = Text(L"Input and output are required.", L"入力と変換後の両方を入れてください。");
            return false;
        }
        const auto previous = dictionary_;
        const auto id = update ? static_cast<std::uint64_t>(message.Number(L"id")) : NextDictionaryId();
        if (update && id == 0) {
            error = Text(L"The dictionary entry was not found.", L"辞書の単語が見つかりませんでした。");
            return false;
        }
        if (update) (void)dictionary_.Remove(id);
        if (!dictionary_.Add({id, raw, candidate,
                              PolicyFlagsFromName(message.String(L"policy")),
                              message.Bool(L"caseSensitive"),
                              message.Bool(L"enabled", true)}) ||
            !repository_->Save(dictionary_)) {
            dictionary_ = previous;
            error = Text(L"The dictionary entry could not be saved.", L"辞書の単語を保存できませんでした。");
            return false;
        }
        runtime_->NotifyDictionaryChanged();
        return true;
    }

    void HandleWebMessage(std::wstring_view message) {
        HandleRequest(message);
        seenGenerations_ = CurrentGenerations();
    }

    void HandleRequest(std::wstring_view json) {
        const auto message = WebMessage::Parse(json);
        const auto type = message.String(L"type");
        const auto requestId = message.String(L"requestId");
        if (type == L"settings.get") {
            Reply(requestId, true);
        } else if (type == L"settings.set") {
            const auto key = message.String(L"key");
            const bool value = message.Bool(L"value");
            const int integerValue = static_cast<int>(message.Number(L"value"));
            const auto previous = settings_;
            bool known = true;
            if (key == L"restoreLastInputMode") settings_.restoreLastInputMode = value;
            else if (key == L"correctionEnabled") settings_.correctionEnabled = value;
            else if (key == L"commonMisspellingsEnabled") settings_.commonMisspellingsEnabled = value;
            else if (key == L"contextSuggestionsEnabled") settings_.contextSuggestionsEnabled = value;
            else if (key == L"completionEnabled") settings_.completionEnabled = value;
            else if (key == L"candidateWindowEnabled") settings_.candidateWindowEnabled = value;
            else if (key == L"learningEnabled") settings_.learningEnabled = value;
            else if (key == L"japanesePhoneticSuggestionsEnabled") {
                settings_.japanesePhoneticSuggestionsEnabled = value;
            }
            else if (key == L"socialExpressionRange") {
                settings_.socialExpressionRange = std::clamp(integerValue, 0, 4);
            }
            else if (key == L"socialPersonalization") {
                settings_.socialPersonalization = std::clamp(integerValue, 0, 2);
            }
            else if (key == L"candidateWindowStyle") {
                settings_.candidateWindowStyle = std::clamp(integerValue, 0, 1);
            }
            else if (key == L"candidateRows") {
                settings_.candidateRows = integerValue == 5 || integerValue == 7 || integerValue == 9 ? integerValue : 0;
            }
            else if (key == L"periodOnEnter") settings_.periodOnEnter = value;
            else if (key == L"meaningsEnabled") settings_.meaningsEnabled = value;
            else if (key == L"japaneseEnabled") settings_.japaneseEnabled = value;
            else if (key == L"modeIndicatorEnabled") settings_.modeIndicatorEnabled = value;
            else if (key == L"japaneseSwitchOrder") {
                settings_.japaneseSwitchOrder = std::clamp(integerValue, 0, 2);
            }
            else if (key == L"japaneseSpaceWidth") {
                settings_.japaneseSpaceWidth = std::clamp(integerValue, 0, 2);
            }
            else if (key == L"japanesePunctuation") {
                settings_.japanesePunctuation = std::clamp(integerValue, 0, 3);
            }
            else if (key == L"japanesePredictionEnabled") settings_.japanesePredictionEnabled = value;
            else if (key == L"uiLanguage") settings_.uiLanguage = std::clamp(integerValue, 0, 2);
            else if (key == L"toggleKey") {
                settings_.toggleKey = std::clamp(integerValue, 0, 3);
            }
            else if (key == L"keyboardType") {
                settings_.keyboardType = std::clamp(integerValue, 0, 2);
            }
            else known = false;
            if (!known) {
                settings_ = previous;
                Reply(requestId, false, Text(L"Unknown setting or startup entry could not be updated.", L"この設定は変更できません。"));
            } else if (!SaveSettings()) {
                settings_ = previous;
                Reply(requestId, false, Text(L"Settings could not be saved.", L"設定を保存できませんでした。"));
            } else {
                if (key == L"uiLanguage") SetWindowTextW(hwnd_, WindowTitle());
                // The English profile's keyboard layout is part of its
                // registration, which needs administrator rights.
                if (key == L"keyboardType" && settings_.keyboardType != previous.keyboardType) {
                    ReregisterInputMethod();
                }
                // Turning Japanese on starts typing Japanese; turning it off
                // leaves Japanese for the English mode used last.
                if (key == L"japaneseEnabled" && japaneseAvailable_ &&
                    settings_.japaneseEnabled != previous.japaneseEnabled) {
                    std::wstring ignored;
                    (void)SetCurrentJapaneseMode(settings_.japaneseEnabled ? tekito::InputMode::Japanese
                                                                           : settings_.japaneseProfileEnglishMode,
                                                 ignored);
                }
                Reply(requestId, true);
            }
        } else if (type == L"mode.set") {
            std::wstring error;
            const auto value = message.String(L"value");
            const auto mode = value == L"direct"     ? tekito::InputMode::Direct
                              : value == L"japanese" ? tekito::InputMode::Japanese
                                                     : tekito::InputMode::Convert;
            (void)DataPacksJson();
            const bool saved = mode == tekito::InputMode::Japanese && !settings_.japaneseEnabled ? false
                               : japaneseAvailable_ ? SetCurrentJapaneseMode(mode, error)
                               : mode == tekito::InputMode::Japanese ? false
                                                                     : SetCurrentMode(mode, error);
            Reply(requestId, saved, error);
        } else if (type == L"excludedApps.add") {
            std::wstring error;
            const bool added = AddExcludedApp(message.String(L"name"), error);
            Reply(requestId, added, error);
        } else if (type == L"excludedApps.browse") {
            const auto path = ChooseExecutable();
            std::wstring error;
            const bool added = path.empty() || AddExcludedApp(path.wstring(), error);
            Reply(requestId, added, error);
        } else if (type == L"excludedApps.remove") {
            const auto name = message.String(L"name");
            const auto previous = settings_;
            std::erase_if(settings_.excludedApps, [&](const std::wstring& other) {
                return _wcsicmp(other.c_str(), name.c_str()) == 0;
            });
            if (!SaveSettings()) {
                settings_ = previous;
                Reply(requestId, false, Text(L"The app list could not be saved.", L"アプリの一覧を保存できませんでした。"));
            } else {
                Reply(requestId, true);
            }
        } else if (type == L"learning.clear") {
            // Clears everything learned from the user's choices: word
            // preferences and the expressions they tend to pick.
            if (!repository_ || !repository_->ResetLearning() || !repository_->ResetSocialLearning()) {
                Reply(requestId, false, Text(L"Learning data could not be reset.", L"学習データを消去できませんでした。"));
            } else {
                learning_.Reset();
                runtime_->NotifyLearningChanged();
                Reply(requestId, true);
            }
        } else if (type == L"japaneseLearning.clear") {
            // Only what was learned from Japanese conversion; English
            // learning has its own button.
            if (!repository_ || !repository_->ResetJapaneseLearning()) {
                Reply(requestId, false, Text(L"Learning data could not be reset.", L"学習データを消去できませんでした。"));
            } else {
                runtime_->NotifyLearningChanged();
                Reply(requestId, true);
            }
        } else if (type == L"dictionary.create" || type == L"dictionary.update") {
            std::wstring error;
            const bool saved = SaveDictionaryFromMessage(message, type == L"dictionary.update", error);
            Reply(requestId, saved, error);
        } else if (type == L"japaneseWords.save") {
            std::wstring error;
            const bool saved = SaveJapaneseWordFromMessage(message, error);
            Reply(requestId, saved, error);
        } else if (type == L"japaneseWords.delete") {
            auto words = japaneseWords_;
            const auto reading = message.String(L"reading");
            const auto surface = message.String(L"surface");
            std::erase_if(words, [&](const tekito::japanese::UserWord& word) {
                return word.reading == reading && word.surface == surface;
            });
            if (!repository_->SaveJapaneseUserWords(words)) {
                Reply(requestId, false, Text(L"The word could not be deleted.", L"単語を削除できませんでした。"));
            } else {
                japaneseWords_ = std::move(words);
                runtime_->NotifyDictionaryChanged();
                Reply(requestId, true);
            }
        } else if (type == L"dictionary.delete") {
            const auto previous = dictionary_;
            if (!dictionary_.Remove(static_cast<std::uint64_t>(message.Number(L"id"))) || !repository_->Save(dictionary_)) {
                dictionary_ = previous;
                Reply(requestId, false, Text(L"The dictionary entry could not be deleted.", L"辞書の単語を削除できませんでした。"));
            } else {
                runtime_->NotifyDictionaryChanged();
                Reply(requestId, true);
            }
        } else if (type == L"dictionary.import") {
            const auto path = ChooseFile(false);
            tekito::UserDictionary imported;
            if (!path.empty() && tekito::userdata::ImportUserDictionary(path, imported) &&
                repository_->Save(imported)) {
                dictionary_ = std::move(imported);
                runtime_->NotifyDictionaryChanged();
                Reply(requestId, true);
            } else if (path.empty()) {
                Reply(requestId, true);
            } else {
                Reply(requestId, false, Text(L"The dictionary could not be imported.", L"辞書を読み込めませんでした。"));
            }
        } else if (type == L"dictionary.export") {
            const auto path = ChooseFile(true);
            if (path.empty()) {
                Reply(requestId, true);
            } else {
                const bool exported = tekito::userdata::ExportUserDictionary(dictionary_, path);
                Reply(requestId, exported, exported ? L"" : Text(L"The dictionary could not be exported.", L"辞書を書き出せませんでした。"));
            }
        } else if (type == L"diagnostics.copy") {
            const auto text = Diagnostics();
            const bool copied = CopyToClipboard(hwnd_, text);
            Reply(requestId, copied, copied ? L"" : Text(L"Diagnostics could not be copied.", L"診断情報をコピーできませんでした。"));
        } else if (type == L"logs.open") {
            const auto path = LogFolder();
            const auto result = path.empty() ? 0 : reinterpret_cast<INT_PTR>(ShellExecuteW(
                hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The log folder could not be opened.", L"ログのフォルダーを開けませんでした。"));
        } else if (type == L"license.get") {
            const auto text = LicenseText();
            Reply(requestId, !text.empty(), text.empty() ? Text(L"The license text could not be loaded.", L"ライセンスの本文を読み込めませんでした。") : L"",
                  L",\"text\":\"" + JsonEscape(text) + L"\"");
        } else if (type == L"website.open") {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
                hwnd_, L"open", kPublisherWebsite, nullptr, nullptr, SW_SHOWNORMAL));
            Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The website could not be opened.", L"Web サイトを開けませんでした。"));
        } else if (type == L"notices.open") {
            const auto path = tekito::ExternalLexiconProvider::DataPackRoot();
            const auto result = path.empty() ? 0 : reinterpret_cast<INT_PTR>(ShellExecuteW(
                hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            Reply(requestId, result > 32, result > 32 ? L"" : Text(L"The data folder could not be opened.", L"データのフォルダーを開けませんでした。"));
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<SettingsApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<SettingsApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        if (message == WM_CREATE) {
            self->seenGenerations_ = self->CurrentGenerations();
            self->ApplyTheme();
            self->CreateWebViewStatusControls();
            self->InitializeWebView();
            SetTimer(hwnd, kStateWatchTimer, kStateWatchIntervalMs, nullptr);
            return 0;
        }
        if (message == WM_TIMER && wParam == kStateWatchTimer) {
            self->CheckForExternalChanges();
            return 0;
        }
        if (message == WM_SETTINGCHANGE && lParam &&
            std::wstring_view(reinterpret_cast<const wchar_t*>(lParam)) == L"ImmersiveColorSet") {
            self->ApplyTheme();
            self->PostState();  // the accent color may have changed
            return 0;
        }
        if (message == WM_SIZE) {
            self->ResizeWebView();
            return 0;
        }
        if (message == WM_GETMINMAXINFO) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = 760;
            limits->ptMinTrackSize.y = 520;
            return 0;
        }
        if (message == WM_COMMAND) {
            if (LOWORD(wParam) == kRetryWebView && HIWORD(wParam) == BN_CLICKED) {
                self->InitializeWebView();
            }
            return 0;
        }
        if (message == WM_DESTROY) {
            KillTimer(hwnd, kStateWatchTimer);
            self->ResetWebView();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }


    std::filesystem::path ChooseFile(bool save) {
        wchar_t buffer[MAX_PATH]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrFilter = L"TEKITO User Dictionary (*.tsv)\0*.tsv\0All files\0*.*\0";
        dialog.Flags = OFN_PATHMUSTEXIST |
                       (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        return (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))
                   ? std::filesystem::path(buffer)
                   : std::filesystem::path{};
    }

    std::filesystem::path ChooseExecutable() {
        wchar_t buffer[MAX_PATH]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFile = buffer;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrFilter = L"Apps (*.exe)\0*.exe\0";
        dialog.lpstrTitle = Text(L"Choose an app where TEKITO stays off", L"TEKITO をオフにするアプリを選ぶ");
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        return GetOpenFileNameW(&dialog) ? std::filesystem::path(buffer)
                                         : std::filesystem::path{};
    }

    std::uint64_t NextDictionaryId() const {
        std::uint64_t id = 1;
        for (const auto& entry : dictionary_.Entries()) id = std::max(id, entry.id + 1);
        return id;
    }

    // Plain-text report for bug reports. It names components, versions and
    // paths only; nothing the user typed is in it.
    std::wstring Diagnostics() const {
        const auto root = tekito::ExternalLexiconProvider::DataPackRoot();
        const auto registeredDll = RegisteredTsfDll();
        const bool registered = !registeredDll.empty() && std::filesystem::exists(registeredDll);
        std::wstring text = L"TEKITO\r\nVersion " + std::wstring(kVersion) + L"\r\n\r\n";
        text += L"Input method: " + std::wstring(registered ? L"Registered" : L"Not registered");
        text += L"\r\nLearning: " + std::wstring(settings_.learningEnabled ? L"On" : L"Off") + L" (" +
                std::to_wstring(learning_.Entries().size() + learning_.PreferenceCount()) +
                L" preferences)";
        text += L"\r\n\r\nData Packs";
        for (const auto& pack : kDataPacks) {
            tekito::userdata::DataPackStatus status;
            const bool valid = tekito::userdata::ValidateDataPack(root / pack.folder, status);
            text += L"\r\n" + std::wstring(pack.folder) + L": " +
                    (valid ? status.version : L"Unavailable (" + status.reason + L")");
        }
        text += L"\r\n\r\nPaths\r\nData: " + root.wstring();
        text += L"\r\nUser data: " + tekito::userdata::DefaultUserDatabasePath().wstring();
        text += L"\r\nInput method DLL: " + (registeredDll.empty() ? L"-" : registeredDll);
        text += L"\r\nLog folder: " + LogFolder().wstring();
        return text;
    }

    // Messages shown to the user follow UserSettings::uiLanguage.
    bool Japanese() const { return tekito::userdata::UseJapaneseUi(settings_.uiLanguage); }
    const wchar_t* Text(const wchar_t* english, const wchar_t* japanese) const {
        return Japanese() ? japanese : english;
    }
    const wchar_t* WindowTitle() const { return Text(L"TEKITO Settings", L"TEKITO 設定"); }

    // Registers the input method again (regsvr32, elevated) so a changed
    // keyboard type reaches the English profile's layout. Declining the
    // prompt keeps the old layout until TEKITO is next installed.
    void ReregisterInputMethod() {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return;
        const auto dll =
            std::filesystem::path(modulePath, modulePath + length).parent_path() / L"Tekito.Tsf.dll";
        wchar_t system[MAX_PATH]{};
        if (!std::filesystem::exists(dll) || GetSystemDirectoryW(system, MAX_PATH) == 0) return;
        const auto regsvr32 = std::filesystem::path(system) / L"regsvr32.exe";
        const std::wstring parameters = L"/s \"" + dll.wstring() + L"\"";
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.hwnd = hwnd_;
        info.lpVerb = L"runas";
        info.lpFile = regsvr32.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) return;
        if (info.hProcess) {
            WaitForSingleObject(info.hProcess, 30000);
            CloseHandle(info.hProcess);
        }
    }

    bool SaveSettings() {
        if (runtime_) settings_.lastInputMode = runtime_->Mode();
        const bool saved = repository_ && repository_->SaveSettings(settings_);
        if (saved && runtime_) runtime_->NotifySettingsChanged();
        return saved;
    }

    bool SetCurrentMode(tekito::InputMode mode, std::wstring& error) {
        const auto previousSettings = settings_;
        const auto previousMode = runtime_ ? runtime_->Mode() : settings_.lastInputMode;
        if (runtime_) runtime_->SetMode(mode);
        settings_.lastInputMode = mode;
        if (!repository_ || !repository_->SaveSettings(settings_)) {
            settings_ = previousSettings;
            if (runtime_) runtime_->SetMode(previousMode);
            error = Text(L"The current mode could not be saved.", L"入力モードを保存できませんでした。");
            return false;
        }
        if (runtime_) runtime_->NotifySettingsChanged();
        return true;
    }


    bool SetCurrentJapaneseMode(tekito::InputMode mode, std::wstring& error) {
        const auto previousSettings = settings_;
        const auto previousMode = runtime_ ? runtime_->JapaneseMode() : settings_.lastJapaneseProfileMode;
        if (runtime_) runtime_->SetJapaneseMode(mode);
        settings_.lastJapaneseProfileMode = mode;
        if (mode != tekito::InputMode::Japanese) settings_.japaneseProfileEnglishMode = mode;
        if (!repository_ || !repository_->SaveSettings(settings_)) {
            settings_ = previousSettings;
            if (runtime_) runtime_->SetJapaneseMode(previousMode);
            error = Text(L"The current mode could not be saved.", L"入力モードを保存できませんでした。");
            return false;
        }
        if (runtime_) runtime_->NotifySettingsChanged();
        return true;
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    HWND fallbackLabel_{};
    HWND fallbackRetry_{};

    std::unique_ptr<tekito::userdata::IUserDataRepository> repository_;
    std::unique_ptr<tekito::userdata::RuntimeModeState> runtime_;
    tekito::UserDictionary dictionary_;
    std::vector<tekito::japanese::UserWord> japaneseWords_;
    tekito::UserLearningStore learning_;
    tekito::userdata::UserSettings settings_{};
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    EventRegistrationToken webMessageToken_{};
    EventRegistrationToken navigationToken_{};
    std::wstring dataPackSummary_{L"Unavailable"};
    std::wstring japaneseDataSummary_{L"0 / 0"};
    bool japaneseAvailable_{false};
    std::wstring dataPacksJson_;
    ComPtr<ICoreWebView2_3> webview3_;
    Generations seenGenerations_{};
};

}  // namespace

int RunTekitoSettings(HINSTANCE instance) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\TEKITO.Settings.Singleton");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kClassName, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (singleInstance) CloseHandle(singleInstance);
        if (comInitialized) CoUninitialize();
        return 0;
    }
    SettingsApp app;
    if (!app.Initialize(instance)) {
        MessageBoxW(nullptr, L"TEKITO Settings failed to start.", L"TEKITO",
                    MB_OK | MB_ICONERROR);
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
        if (comInitialized) CoUninitialize();
        return 1;
    }
    const int result = app.Run();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    if (comInitialized) CoUninitialize();
    return result;
}
