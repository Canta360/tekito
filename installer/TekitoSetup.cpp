// TEKITO single-EXE setup. The installer package (a zip) is appended to this
// executable; the wizard extracts it, verifies it and runs install.ps1.
//
// The UI is drawn with Direct2D/DirectWrite on the Windows 11 Mica backdrop,
// in the same language as the candidate window and Settings: ink on glass,
// color only where something is on or selected.
#include <windows.h>
#include <windowsx.h>
#include <d2d1_3.h>
#include <dwmapi.h>
#include <dwrite_3.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include "WebView2.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// The wizard speaks Japanese when Windows does. Preview builds can force it
// with TEKITO_PREVIEW_LANGUAGE=ja or =en.
bool UseJapanese() {
    static const bool japanese = [] {
#ifdef TEKITO_SETUP_PREVIEW
        wchar_t value[8]{};
        if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_LANGUAGE", value, 8) > 0) {
            return wcscmp(value, L"ja") == 0;
        }
#endif
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
    }();
    return japanese;
}

const wchar_t* Tr(const wchar_t* english, const wchar_t* japanese) {
    return UseJapanese() ? japanese : english;
}

constexpr char kPayloadMagic[] = "TEKITO_PAYLOAD_V1";
constexpr std::size_t kPayloadMagicSize = sizeof(kPayloadMagic) - 1;
constexpr int kLogoIcon = 101;
constexpr int kLicenseResource = 401;
constexpr int kWordmarkResource = 501;
constexpr int kFontResource = 301;  // M PLUS 1, the TEKITO typeface
constexpr UINT kWizardInstallComplete = WM_APP + 1;
constexpr UINT_PTR kAnimationTimer = 1;

// Layout, in DIPs.
constexpr float kWidth = 600.0f;
constexpr float kHeight = 460.0f;
constexpr float kMargin = 40.0f;
constexpr float kFooterTop = 400.0f;
constexpr float kButtonHeight = 36.0f;

template <typename T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : pointer_(other.pointer_) { other.pointer_ = nullptr; }
    Com& operator=(Com&& other) noexcept {
        if (this != &other) {
            Reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }
    ~Com() { Reset(); }
    T* Get() const { return pointer_; }
    T** Put() { Reset(); return &pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }
    void Reset() {
        if (pointer_) pointer_->Release();
        pointer_ = nullptr;
    }

private:
    T* pointer_{nullptr};
};

enum class Screen { Welcome, License, Options, Installing, Done, Failed };

enum class Action {
    Install, Cancel, Options, ReadLicense, Agree, GetRuntime, CheckRuntime,
    Back, AcceptLicense, Browse, Shortcut, Finish, Retry, Close, Japanese, KeepEnglish,
    WithoutJapanese,
};

enum class Kind { Primary, Secondary, Quiet, Checkbox, Toggle, Link };

struct Control {
    Action action;
    Kind kind;
    D2D1_RECT_F rect;
    std::wstring label;
    bool enabled{true};
    bool checked{false};
};

struct Palette {
    D2D1_COLOR_F background;  // used where Mica is unavailable
    D2D1_COLOR_F ink;
    D2D1_COLOR_F ink2;
    D2D1_COLOR_F ink3;
    D2D1_COLOR_F card;
    D2D1_COLOR_F hairline;
    D2D1_COLOR_F recess;
    D2D1_COLOR_F primary;
    D2D1_COLOR_F primaryInk;
    D2D1_COLOR_F problem;
};

constexpr Palette kLight{
    {0.953f, 0.961f, 0.973f, 1.0f}, {0.059f, 0.090f, 0.125f, 1.0f}, {0.294f, 0.341f, 0.392f, 1.0f},
    {0.494f, 0.533f, 0.580f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.72f},      {0.059f, 0.090f, 0.125f, 0.09f},
    {0.059f, 0.090f, 0.125f, 0.07f}, {0.067f, 0.094f, 0.125f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
    {0.761f, 0.255f, 0.047f, 1.0f},
};

constexpr Palette kDark{
    {0.086f, 0.098f, 0.118f, 1.0f}, {0.918f, 0.933f, 0.949f, 1.0f}, {0.635f, 0.678f, 0.722f, 1.0f},
    {0.431f, 0.471f, 0.514f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.06f},      {1.0f, 1.0f, 1.0f, 0.08f},
    {0.0f, 0.0f, 0.0f, 0.28f},       {0.933f, 0.945f, 0.957f, 1.0f}, {0.047f, 0.063f, 0.078f, 1.0f},
    {0.984f, 0.573f, 0.235f, 1.0f},
};

#pragma pack(push, 1)
struct PayloadTrailer {
    char magic[kPayloadMagicSize];
    std::uint64_t payloadSize;
    std::uint64_t reserved;
};
#pragma pack(pop)

struct Wizard {
    HWND window{};
    std::filesystem::path self;
    Screen screen{Screen::Welcome};
    std::filesystem::path installRoot;
    bool startMenuShortcut{true};
    // Japanese input: its data is downloaded from the release on GitHub.
    // On by default where Windows shows Japanese.
    bool japanese{UseJapanese()};
    // With Japanese: keep TEKITO in the English keyboard list too (the
    // Japanese one has Auto and Direct for English).
    bool keepEnglish{false};
    bool agreed{false};
    bool webViewReady{false};
    std::thread installThread;
    std::atomic<int> installExitCode{1};
    bool completed{false};
    float animation{0.0f};

    // Interaction.
    int hover{-1};
    int focus{0};
    bool keyboardFocus{false};
    bool pressed{false};
    float licenseScroll{0.0f};

    // Rendering.
    UINT dpi{96};
    bool dark{false};
    bool mica{false};
    D2D1_COLOR_F accent{0.0f, 0.471f, 0.831f, 1.0f};
    Com<ID2D1Factory1> factory;
    Com<IDWriteFactory> dwrite;
    Com<IDWriteInMemoryFontFileLoader> fontLoader;
    Com<IDWriteFontCollection1> fonts;  // M PLUS 1; null falls back to Segoe UI
    Com<ID2D1HwndRenderTarget> target;
    Com<ID2D1Bitmap> icon;
    Com<ID2D1SvgDocument> wordmark;
    Com<IDWriteTextFormat> title;
    Com<IDWriteTextFormat> heading;
    Com<IDWriteTextFormat> headingCentered;
    Com<IDWriteTextFormat> bodyCentered;
    Com<IDWriteTextFormat> captionCentered;
    Com<IDWriteTextFormat> body;
    Com<IDWriteTextFormat> caption;
    Com<IDWriteTextFormat> button;
    Com<IDWriteTextFormat> mono;
    Com<IDWriteTextFormat> licenseFormat;
    Com<IDWriteTextLayout> license;
    std::wstring licenseText;
    std::vector<DWRITE_TEXT_RANGE> licenseHeadings;
};

const Palette& Colors(const Wizard& wizard) { return wizard.dark ? kDark : kLight; }

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}

// ---------------------------------------------------------------------------
// System queries.

bool AppsUseDarkTheme() {
#ifdef TEKITO_SETUP_PREVIEW
    wchar_t theme[16]{};
    if (GetEnvironmentVariableW(L"TEKITO_PREVIEW_THEME", theme, 16) > 0) return wcscmp(theme, L"dark") == 0;
#endif
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value == 0;
}

D2D1_COLOR_F AccentColor() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) {
        return {0.0f, 0.471f, 0.831f, 1.0f};
    }
    return {(value & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, ((value >> 16) & 0xff) / 255.0f, 1.0f};
}

std::wstring QuotePowerShell(std::wstring value) {
    std::wstring quoted = L"'";
    for (const wchar_t ch : value) {
        if (ch == L'\'') quoted += L"''";
        else quoted.push_back(ch);
    }
    quoted.push_back(L'\'');
    return quoted;
}

bool HasWebView2Runtime() {
    LPWSTR version = nullptr;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version) {
        CoTaskMemFree(version);
        return true;
    }
    if (version) CoTaskMemFree(version);

    constexpr wchar_t kWebView2Client[] =
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4A2A-8C2B-7D8D7B5B4A3F}";
    const auto hasRegistryVersion = [&](HKEY root, REGSAM view) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kWebView2Client, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t found[64]{};
        DWORD type = 0;
        DWORD size = sizeof(found);
        const auto result = RegQueryValueExW(key, L"pv", nullptr, &type, reinterpret_cast<LPBYTE>(found), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_SZ && found[0] != L'\0' &&
               std::wstring_view(found) != L"0.0.0.0";
    };
    for (const auto root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        if (hasRegistryVersion(root, KEY_WOW64_64KEY) || hasRegistryVersion(root, KEY_WOW64_32KEY)) {
            return true;
        }
    }
    return false;
}

void OpenWebView2DownloadPage() {
    ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr,
                  SW_SHOWNORMAL);
}

std::filesystem::path DefaultInstallRoot() {
    wchar_t localAppData[MAX_PATH]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(localAppData) / L"Programs" / L"TEKITO";
}

std::filesystem::path BrowseForDirectory(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    browse.lpszTitle = Tr(L"Choose where to install TEKITO", L"TEKITO のインストール先を選んでください");
    const auto selected = SHBrowseForFolderW(&browse);
    if (!selected) return {};
    wchar_t path[MAX_PATH]{};
    const bool read = SHGetPathFromIDListW(selected, path) != FALSE;
    CoTaskMemFree(selected);
    return read ? std::filesystem::path(path) : std::filesystem::path{};
}

bool ResourceBytes(int id, const BYTE*& bytes, DWORD& size) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    size = resource ? SizeofResource(module, resource) : 0;
    bytes = loaded && size ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    return bytes != nullptr;
}

// The license is Markdown; strip the markup that would read as noise and
// remember which lines were headings.
void LoadLicenseText(Wizard& wizard) {
    wizard.licenseText.clear();
    wizard.licenseHeadings.clear();
    const BYTE* bytes = nullptr;
    DWORD size = 0;
    std::wstring text;
    if (ResourceBytes(kLicenseResource, bytes, size)) {
        const auto* chars = reinterpret_cast<const char*>(bytes);
        const int count = MultiByteToWideChar(CP_UTF8, 0, chars, static_cast<int>(size), nullptr, 0);
        text.resize(static_cast<std::size_t>(std::max(count, 0)));
        MultiByteToWideChar(CP_UTF8, 0, chars, static_cast<int>(size), text.data(), count);
    }
    if (text.empty()) text = Tr(L"The license text could not be loaded. See the license file in the TEKITO package.", L"使用許諾契約を読み込めませんでした。TEKITO のパッケージに含まれるライセンスファイルをご覧ください。");

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(L'\n', start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line.front() == L'>') line.erase(0, line.size() > 1 && line[1] == L' ' ? 2 : 1);
        bool heading = false;
        while (!line.empty() && line.front() == L'#') {
            heading = true;
            line.erase(line.begin());
        }
        if (heading && !line.empty() && line.front() == L' ') line.erase(line.begin());
        for (const wchar_t* marker : {L"**", L"`"}) {
            for (std::size_t at = 0; (at = line.find(marker, at)) != std::wstring::npos;) line.erase(at, wcslen(marker));
        }
        if (heading && !line.empty()) {
            wizard.licenseHeadings.push_back({static_cast<UINT32>(wizard.licenseText.size()),
                                              static_cast<UINT32>(line.size())});
        }
        wizard.licenseText += line;
        wizard.licenseText += L'\n';
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
}

// ---------------------------------------------------------------------------
// Install.

bool ReadPayloadBounds(const std::filesystem::path& self, std::uint64_t& payloadOffset, std::uint64_t& payloadSize) {
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(self, error);
    if (error || fileSize < sizeof(PayloadTrailer)) return false;
    std::ifstream input(self, std::ios::binary);
    if (!input) return false;
    input.seekg(static_cast<std::streamoff>(fileSize - sizeof(PayloadTrailer)));
    PayloadTrailer trailer{};
    input.read(reinterpret_cast<char*>(&trailer), sizeof(trailer));
    if (!input || std::memcmp(trailer.magic, kPayloadMagic, kPayloadMagicSize) != 0) return false;
    if (trailer.payloadSize > fileSize - sizeof(PayloadTrailer)) return false;
    payloadSize = trailer.payloadSize;
    payloadOffset = fileSize - sizeof(PayloadTrailer) - payloadSize;
    return true;
}

bool CopyPayload(const std::filesystem::path& self, std::uint64_t payloadOffset, std::uint64_t payloadSize,
                 const std::filesystem::path& destination) {
    std::ifstream input(self, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output) return false;
    input.seekg(static_cast<std::streamoff>(payloadOffset));
    std::vector<char> buffer(1024 * 1024);
    while (payloadSize != 0) {
        const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(payloadSize, buffer.size()));
        input.read(buffer.data(), chunk);
        if (input.gcount() != chunk) return false;
        output.write(buffer.data(), chunk);
        if (!output) return false;
        payloadSize -= static_cast<std::uint64_t>(chunk);
    }
    return true;
}

std::filesystem::path MakeTempDirectory() {
    wchar_t tempPath[MAX_PATH]{};
    const auto length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) return {};
    for (unsigned int attempt = 0; attempt < 10; ++attempt) {
        const auto candidate = std::filesystem::path(tempPath) /
                               (L"TEKITO-Setup-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error) && !error) return candidate;
    }
    return {};
}

struct InstallChoices {
    std::filesystem::path installRoot;
    bool startMenuShortcut{true};
    bool japanese{false};
    bool keepEnglish{false};
};

int RunPowerShell(const std::filesystem::path& zipPath, const std::filesystem::path& packageRoot,
                  const InstallChoices& choices) {
    wchar_t systemDirectory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return -1;
    const auto powershell = std::filesystem::path(systemDirectory) / LR"(WindowsPowerShell\v1.0\powershell.exe)";
    // Unpack, verify the package against its manifest, then install;
    // install.ps1's own exit codes (Japanese data not downloaded, ...) are
    // passed on.
    const auto flag = [](bool value) { return std::wstring(value ? L"$true" : L"$false"); };
    const auto command = L"-NoProfile -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; "
                         L"Expand-Archive -LiteralPath " + QuotePowerShell(zipPath.wstring()) +
                         L" -DestinationPath " + QuotePowerShell(packageRoot.wstring()) +
                         L" -Force; & " + QuotePowerShell((packageRoot / L"verify-package.ps1").wstring()) +
                         L" -PackageRoot " + QuotePowerShell(packageRoot.wstring()) +
                         L"; & " + QuotePowerShell((packageRoot / L"install.ps1").wstring()) +
                         L" -SourceRoot " + QuotePowerShell(packageRoot.wstring()) +
                         L" -InstallRoot " + QuotePowerShell(choices.installRoot.wstring()) +
                         L" -StartMenuShortcut:" + flag(choices.startMenuShortcut) +
                         L" -Japanese:" + flag(choices.japanese) +
                         L" -KeepEnglishProfile:" + flag(choices.keepEnglish) +
                         L"; exit $(if ($LASTEXITCODE) { $LASTEXITCODE } else { 0 })\"";
    std::vector<wchar_t> commandLine(command.begin(), command.end());
    commandLine.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(powershell.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        packageRoot.c_str(), &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

int InstallEmbeddedPackage(const std::filesystem::path& self, const InstallChoices& choices) {
    std::uint64_t payloadOffset = 0;
    std::uint64_t payloadSize = 0;
    if (!ReadPayloadBounds(self, payloadOffset, payloadSize)) return 2;
    const auto tempRoot = MakeTempDirectory();
    if (tempRoot.empty()) return 3;
    const auto zipPath = tempRoot / L"TEKITO-package.zip";
    const auto packageRoot = tempRoot / L"package";
    std::error_code cleanupError;
    if (!CopyPayload(self, payloadOffset, payloadSize, zipPath)) {
        std::filesystem::remove_all(tempRoot, cleanupError);
        return 4;
    }
    std::filesystem::create_directory(packageRoot, cleanupError);
    const auto exitCode = RunPowerShell(zipPath, packageRoot, choices);
    std::filesystem::remove_all(tempRoot, cleanupError);
    return exitCode == -1 ? 5 : exitCode;
}

std::wstring FailureMessage(int code) {
    switch (code) {
    case 2: return Tr(L"This setup file is incomplete or damaged. Download TEKITO again and run the new file.", L"このセットアップファイルは不完全か、壊れています。TEKITO をダウンロードし直して、新しいファイルを実行してください。");
    case 3: return Tr(L"Setup couldn't create a temporary folder. Make sure the drive has free space, then try again.", L"一時フォルダーを作成できませんでした。ドライブの空き容量を確認して、もう一度お試しください。");
    case 4: return Tr(L"Setup couldn't unpack its files. Make sure the drive has free space, then try again.", L"ファイルを展開できませんでした。ドライブの空き容量を確認して、もう一度お試しください。");
    case 5: return Tr(L"Setup couldn't start Windows PowerShell, which it needs to install TEKITO.", L"インストールに必要な Windows PowerShell を起動できませんでした。");
    case 20: return Tr(L"Setup couldn't download the Japanese data. Check the internet connection and try again, or install without Japanese.", L"日本語のデータをダウンロードできませんでした。インターネットの接続を確かめてもう一度お試しいただくか、日本語なしでインストールしてください。");
    case 21: return Tr(L"The downloaded Japanese data was not the file this installer expects. Try again, or download the latest TEKITO.", L"ダウンロードした日本語のデータが、このインストーラーの想定と違いました。もう一度お試しいただくか、最新の TEKITO をダウンロードしてください。");
    default:
        if (UseJapanese()) {
            return L"インストールがエラー " + std::to_wstring(code) +
                   L" で止まりました。新しいインストールだった場合、何も残っていません。もう一度お試しください。"
                   L"繰り返し起きる場合は、%TEMP%\\TEKITO-install.log に詳しい記録があります。";
        }
        return L"The install step stopped with error " + std::to_wstring(code) +
               L". If this was a new install, nothing was left behind. Try again; if it keeps happening, "
               L"%TEMP%\\TEKITO-install.log says why.";
    }
}

void StartInstall(Wizard& wizard) {
#ifdef TEKITO_SETUP_PREVIEW
    wizard.screen = Screen::Installing;  // the preview never installs
    SetTimer(wizard.window, kAnimationTimer, 16, nullptr);
    return;
#endif
    wizard.screen = Screen::Installing;
    wizard.installExitCode.store(1);
    SetTimer(wizard.window, kAnimationTimer, 16, nullptr);
    const HWND window = wizard.window;
    const auto self = wizard.self;
    const InstallChoices choices{wizard.installRoot, wizard.startMenuShortcut, wizard.japanese,
                                 wizard.japanese && wizard.keepEnglish};
    try {
        wizard.installThread = std::thread([&wizard, window, self, choices] {
            wizard.installExitCode.store(InstallEmbeddedPackage(self, choices));
            PostMessageW(window, kWizardInstallComplete, 0, 0);
        });
    } catch (...) {
        KillTimer(window, kAnimationTimer);
        wizard.installExitCode.store(5);
        wizard.screen = Screen::Failed;
    }
}

// ---------------------------------------------------------------------------
// Layout.

float TextWidth(const Wizard& wizard, IDWriteTextFormat* format, const std::wstring& text) {
    Com<IDWriteTextLayout> layout;
    if (!format || FAILED(wizard.dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                                          1000.0f, 100.0f, layout.Put()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

D2D1_RECT_F FooterButton(int slotFromRight, float width = 116.0f) {
    const float right = kWidth - 28.0f - slotFromRight * (116.0f + 10.0f);
    return D2D1::RectF(right - width, kFooterTop, right, kFooterTop + kButtonHeight);
}

constexpr D2D1_RECT_F kWebViewCard{kMargin, 226.0f, kWidth - kMargin, 326.0f};
constexpr D2D1_RECT_F kLicenseCard{kMargin, 100.0f, kWidth - kMargin, 384.0f};
constexpr D2D1_RECT_F kOptionsCard{kMargin, 92.0f, kWidth - kMargin, 252.0f};
// With Japanese, the options card has a third row.
constexpr D2D1_RECT_F kOptionsCardJapanese{kMargin, 92.0f, kWidth - kMargin, 318.0f};
// Welcome: the Japanese choice, and the license agreement under it.
float JapaneseRow(const Wizard& wizard) { return wizard.webViewReady ? 238.0f : 334.0f; }
float AgreeRow(const Wizard& wizard) { return wizard.webViewReady ? 336.0f : 366.0f; }

std::vector<Control> Controls(const Wizard& wizard) {
    std::vector<Control> controls;
    switch (wizard.screen) {
    case Screen::Welcome: {
        if (!wizard.webViewReady) {
            const float y = kWebViewCard.top + 58.0f;
            controls.push_back({Action::GetRuntime, Kind::Secondary, D2D1::RectF(58.0f, y, 188.0f, y + 32.0f), Tr(L"Get WebView2", L"WebView2 を入手")});
            controls.push_back({Action::CheckRuntime, Kind::Quiet, D2D1::RectF(196.0f, y, 306.0f, y + 32.0f), Tr(L"Check again", L"もう一度確認")});
        }
        const std::wstring japanese = Tr(L"Add Japanese input", L"日本語入力を追加する");
        const float japaneseRow = JapaneseRow(wizard);
        controls.push_back({Action::Japanese, Kind::Checkbox,
                            D2D1::RectF(kMargin, japaneseRow, kMargin + 30.0f + TextWidth(wizard, wizard.body.Get(), japanese),
                                        japaneseRow + 24.0f),
                            japanese, true, wizard.japanese});
        const std::wstring agree = Tr(L"I agree to the license terms", L"使用許諾契約に同意する");
        const float agreeWidth = TextWidth(wizard, wizard.body.Get(), agree);
        const float row = AgreeRow(wizard);
        controls.push_back({Action::Agree, Kind::Checkbox, D2D1::RectF(kMargin, row, kMargin + 30.0f + agreeWidth, row + 24.0f),
                            agree, true, wizard.agreed});
        const float linkLeft = kMargin + 30.0f + agreeWidth + 12.0f;
        controls.push_back({Action::ReadLicense, Kind::Link, D2D1::RectF(linkLeft, row, linkLeft + 72.0f, row + 24.0f), Tr(L"Read them", L"内容を読む")});
        controls.push_back({Action::Options, Kind::Quiet, D2D1::RectF(22.0f, kFooterTop, 130.0f, kFooterTop + kButtonHeight), Tr(L"Options", L"オプション")});
        controls.push_back({Action::Cancel, Kind::Secondary, FooterButton(1), Tr(L"Cancel", L"キャンセル")});
        controls.push_back({Action::Install, Kind::Primary, FooterButton(0), Tr(L"Install", L"インストール"), wizard.agreed && wizard.webViewReady});
        break;
    }
    case Screen::License:
        controls.push_back({Action::Back, Kind::Secondary, FooterButton(1), Tr(L"Back", L"戻る")});
        controls.push_back({Action::AcceptLicense, Kind::Primary, FooterButton(0), Tr(L"I agree", L"同意する")});
        break;
    case Screen::Options:
        controls.push_back({Action::Browse, Kind::Secondary, D2D1::RectF(kOptionsCard.right - 118.0f, kOptionsCard.top + 20.0f,
                                                                        kOptionsCard.right - 18.0f, kOptionsCard.top + 52.0f),
                            Tr(L"Change", L"変更")});
        controls.push_back({Action::Shortcut, Kind::Toggle, D2D1::RectF(kOptionsCard.right - 66.0f, kOptionsCard.top + 104.0f,
                                                                       kOptionsCard.right - 18.0f, kOptionsCard.top + 132.0f),
                            Tr(L"Start menu shortcut", L"スタートメニューのショートカット"), true, wizard.startMenuShortcut});
        if (wizard.japanese) {
            controls.push_back({Action::KeepEnglish, Kind::Toggle,
                                D2D1::RectF(kOptionsCard.right - 66.0f, kOptionsCard.top + 166.0f,
                                            kOptionsCard.right - 18.0f, kOptionsCard.top + 194.0f),
                                Tr(L"Keep TEKITO for English keyboards", L"英語のキーボードにも TEKITO を残す"), true,
                                wizard.keepEnglish});
        }
        controls.push_back({Action::Back, Kind::Primary, FooterButton(0), Tr(L"Done", L"完了")});
        break;
    case Screen::Installing:
        break;
    case Screen::Done:
        controls.push_back({Action::Finish, Kind::Primary, FooterButton(0), Tr(L"Finish", L"完了")});
        break;
    case Screen::Failed: {
        // Without the Japanese data, English can still be installed.
        const int code = wizard.installExitCode.load();
        if (code == 20 || code == 21) {
            controls.push_back({Action::Close, Kind::Quiet, D2D1::RectF(22.0f, kFooterTop, 130.0f, kFooterTop + kButtonHeight),
                                Tr(L"Close", L"閉じる")});
            const std::wstring without = Tr(L"Install without Japanese", L"日本語なしでインストール");
            const float width = TextWidth(wizard, wizard.body.Get(), without) + 40.0f;
            const D2D1_RECT_F retry = FooterButton(0);
            controls.push_back({Action::WithoutJapanese, Kind::Secondary,
                                D2D1::RectF(retry.left - 12.0f - width, retry.top, retry.left - 12.0f, retry.bottom), without});
        } else {
            controls.push_back({Action::Close, Kind::Secondary, FooterButton(1), Tr(L"Close", L"閉じる")});
        }
        controls.push_back({Action::Retry, Kind::Primary, FooterButton(0), Tr(L"Try again", L"再試行")});
        break;
    }
    }
    return controls;
}

// ---------------------------------------------------------------------------
// Rendering.

// Loads the embedded M PLUS 1 into a private font collection.
void LoadBrandFont(Wizard& wizard) {
    Com<IDWriteFactory5> factory5;
    const BYTE* bytes = nullptr;
    DWORD size = 0;
    if (FAILED(wizard.dwrite->QueryInterface(IID_PPV_ARGS(factory5.Put()))) || !ResourceBytes(kFontResource, bytes, size) ||
        FAILED(factory5->CreateInMemoryFontFileLoader(wizard.fontLoader.Put())) ||
        FAILED(factory5->RegisterFontFileLoader(wizard.fontLoader.Get()))) {
        return;
    }
    Com<IDWriteFontFile> file;
    Com<IDWriteFontSetBuilder1> builder;
    Com<IDWriteFontSet> set;
    if (FAILED(wizard.fontLoader->CreateInMemoryFontFileReference(factory5.Get(), bytes, size, nullptr, file.Put())) ||
        FAILED(factory5->CreateFontSetBuilder(builder.Put())) || FAILED(builder->AddFontFile(file.Get())) ||
        FAILED(builder->CreateFontSet(set.Put()))) {
        return;
    }
    factory5->CreateFontCollectionFromFontSet(set.Get(), wizard.fonts.Put());
}

Com<IDWriteTextFormat> MakeFormat(const Wizard& wizard, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                 DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
                                 DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
    Com<IDWriteTextFormat> format;
    // `family` is the system fallback; the brand face is used when it loaded.
    const bool brand = wizard.fonts && family != nullptr && wcscmp(family, L"Cascadia Mono") != 0;
    if (SUCCEEDED(wizard.dwrite->CreateTextFormat(brand ? L"M PLUS 1" : family, brand ? wizard.fonts.Get() : nullptr, weight,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                                  UseJapanese() ? L"ja-jp" : L"en-us",
                                                  format.Put()))) {
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(paragraph);
    }
    return format;
}

void CreateTextResources(Wizard& wizard) {
    IDWriteFactory* f = wizard.dwrite.Get();
    const Wizard& w = wizard;
    const wchar_t* display = L"Segoe UI Variable Display";
    const wchar_t* text = L"Segoe UI Variable Text";
    wizard.title = MakeFormat(w, display, 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    wizard.heading = MakeFormat(w, display, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    wizard.headingCentered = MakeFormat(w, display, 22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    wizard.bodyCentered = MakeFormat(w, text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    wizard.captionCentered = MakeFormat(w, text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    wizard.body = MakeFormat(w, text, 14.0f, DWRITE_FONT_WEIGHT_NORMAL);
    wizard.caption = MakeFormat(w, text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    wizard.button = MakeFormat(w, text, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,
                               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    wizard.mono = MakeFormat(w, L"Cascadia Mono", 12.0f, DWRITE_FONT_WEIGHT_NORMAL);
    wizard.licenseFormat = MakeFormat(w, text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    if (wizard.mono) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        Com<IDWriteInlineObject> sign;
        if (SUCCEEDED(f->CreateEllipsisTrimmingSign(wizard.mono.Get(), sign.Put()))) {
            wizard.mono->SetTrimming(&trimming, sign.Get());
            wizard.mono->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    wizard.license.Reset();
    if (wizard.licenseFormat) {
        const float width = (kLicenseCard.right - kLicenseCard.left) - 48.0f;
        if (SUCCEEDED(f->CreateTextLayout(wizard.licenseText.c_str(), static_cast<UINT32>(wizard.licenseText.size()),
                                          wizard.licenseFormat.Get(), width, 100000.0f, wizard.license.Put()))) {
            wizard.license->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 19.0f, 15.0f);
            for (const auto& range : wizard.licenseHeadings) {
                wizard.license->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
                wizard.license->SetFontSize(14.0f, range);
            }
        }
    }
}

void ReleaseDeviceResources(Wizard& wizard) {
    wizard.icon.Reset();
    wizard.wordmark.Reset();
    wizard.target.Reset();
}

bool EnsureTarget(Wizard& wizard) {
    if (wizard.target) return true;
    RECT client{};
    GetClientRect(wizard.window, &client);
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(wizard.dpi), static_cast<float>(wizard.dpi));
    if (FAILED(wizard.factory->CreateHwndRenderTarget(
            properties,
            D2D1::HwndRenderTargetProperties(wizard.window, D2D1::SizeU(static_cast<UINT32>(client.right),
                                                                        static_cast<UINT32>(client.bottom))),
            wizard.target.Put()))) {
        return false;
    }
    wizard.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // App icon, from the executable's icon resource.
    Com<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.Put())))) {
        const auto icon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kLogoIcon), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR));
        Com<IWICBitmap> bitmap;
        Com<IWICFormatConverter> converter;
        if (icon && SUCCEEDED(wic->CreateBitmapFromHICON(icon, bitmap.Put())) &&
            SUCCEEDED(wic->CreateFormatConverter(converter.Put())) &&
            SUCCEEDED(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            wizard.target->CreateBitmapFromWicBitmap(converter.Get(), wizard.icon.Put());
        }
        if (icon) DestroyIcon(icon);
    }

    // Wordmark, from the same SVG the Settings window uses.
    Com<ID2D1DeviceContext5> context;
    const BYTE* bytes = nullptr;
    DWORD size = 0;
    if (SUCCEEDED(wizard.target->QueryInterface(IID_PPV_ARGS(context.Put()))) &&
        ResourceBytes(kWordmarkResource, bytes, size)) {
        if (IStream* stream = SHCreateMemStream(bytes, size)) {
            context->CreateSvgDocument(stream, D2D1::SizeF(158.0f, 28.0f), wizard.wordmark.Put());
            stream->Release();
            // The artwork is dark blue; on dark glass draw it in the ink color.
            if (wizard.wordmark && wizard.dark) {
                Com<ID2D1SvgElement> root;
                wizard.wordmark->GetRoot(root.Put());
                const D2D1_COLOR_F ink = Colors(wizard).ink;
                std::vector<ID2D1SvgElement*> stack;
                if (root) {
                    root->AddRef();
                    stack.push_back(root.Get());
                }
                while (!stack.empty()) {
                    ID2D1SvgElement* element = stack.back();
                    stack.pop_back();
                    if (element->IsAttributeSpecified(L"fill")) {
                        element->SetAttributeValue(L"fill", D2D1_SVG_ATTRIBUTE_POD_TYPE_COLOR, &ink, sizeof(ink));
                    }
                    ID2D1SvgElement* child = nullptr;
                    element->GetFirstChild(&child);
                    while (child) {
                        stack.push_back(child);
                        ID2D1SvgElement* next = nullptr;
                        element->GetNextChild(child, &next);
                        child = next;
                    }
                    element->Release();
                }
            }
        }
    }
    return true;
}

Com<ID2D1SolidColorBrush> Brush(const Wizard& wizard, const D2D1_COLOR_F& color) {
    Com<ID2D1SolidColorBrush> brush;
    wizard.target->CreateSolidColorBrush(color, brush.Put());
    return brush;
}

D2D1_ROUNDED_RECT Round(const D2D1_RECT_F& rect, float radius) { return D2D1::RoundedRect(rect, radius, radius); }

void DrawText(const Wizard& wizard, const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
              const D2D1_COLOR_F& color) {
    if (!format) return;
    auto brush = Brush(wizard, color);
    wizard.target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// A frosted card: faint fill, hairline edge, brighter along the top.
void DrawCard(const Wizard& wizard, const D2D1_RECT_F& rect) {
    const auto& p = Colors(wizard);
    auto fill = Brush(wizard, p.card);
    auto edge = Brush(wizard, p.hairline);
    wizard.target->FillRoundedRectangle(Round(rect, 18.0f), fill.Get());
    wizard.target->DrawRoundedRectangle(Round(D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f,
                                                          rect.bottom - 0.5f), 17.5f), edge.Get(), 1.0f);
}

void DrawControl(const Wizard& wizard, const Control& control, bool hovered, bool focused) {
    const auto& p = Colors(wizard);
    ID2D1RenderTarget* t = wizard.target.Get();
    const auto& r = control.rect;
    const float alpha = control.enabled ? 1.0f : 0.4f;
    switch (control.kind) {
    case Kind::Primary: {
        auto fill = Brush(wizard, WithAlpha(p.primary, alpha * (hovered && control.enabled ? 0.88f : 1.0f)));
        t->FillRoundedRectangle(Round(r, (r.bottom - r.top) / 2.0f), fill.Get());
        DrawText(wizard, control.label, wizard.button.Get(), r, WithAlpha(p.primaryInk, control.enabled ? 1.0f : 0.8f));
        break;
    }
    case Kind::Secondary: {
        const auto base = hovered ? p.recess : p.card;
        auto fill = Brush(wizard, WithAlpha(base, base.a * alpha));
        auto edge = Brush(wizard, p.hairline);
        t->FillRoundedRectangle(Round(r, (r.bottom - r.top) / 2.0f), fill.Get());
        t->DrawRoundedRectangle(Round(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                                      (r.bottom - r.top) / 2.0f - 0.5f), edge.Get(), 1.0f);
        DrawText(wizard, control.label, wizard.button.Get(), r, WithAlpha(p.ink, alpha));
        break;
    }
    case Kind::Quiet: {
        if (hovered) {
            auto fill = Brush(wizard, p.recess);
            t->FillRoundedRectangle(Round(r, (r.bottom - r.top) / 2.0f), fill.Get());
        }
        DrawText(wizard, control.label, wizard.button.Get(), r, hovered ? p.ink : p.ink2);
        break;
    }
    case Kind::Link: {
        const auto color = hovered ? p.ink : p.ink2;
        DrawText(wizard, control.label, wizard.body.Get(), D2D1::RectF(r.left, r.top + 2.0f, r.right, r.bottom), color);
        const float width = TextWidth(wizard, wizard.body.Get(), control.label);
        auto line = Brush(wizard, WithAlpha(color, 0.6f));
        t->DrawLine(D2D1::Point2F(r.left, r.top + 21.0f), D2D1::Point2F(r.left + width, r.top + 21.0f), line.Get(), 1.0f);
        break;
    }
    case Kind::Checkbox: {
        const auto box = D2D1::RectF(r.left, r.top + 3.0f, r.left + 18.0f, r.top + 21.0f);
        if (control.checked) {
            auto fill = Brush(wizard, wizard.accent);
            t->FillRoundedRectangle(Round(box, 5.0f), fill.Get());
            auto mark = Brush(wizard, D2D1::ColorF(1.0f, 1.0f, 1.0f));
            Com<ID2D1PathGeometry> path;
            Com<ID2D1GeometrySink> sink;
            if (SUCCEEDED(wizard.factory->CreatePathGeometry(path.Put())) && SUCCEEDED(path->Open(sink.Put()))) {
                sink->BeginFigure(D2D1::Point2F(box.left + 4.5f, box.top + 9.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(box.left + 7.8f, box.top + 12.8f));
                sink->AddLine(D2D1::Point2F(box.left + 13.5f, box.top + 5.8f));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                t->DrawGeometry(path.Get(), mark.Get(), 2.0f);
            }
        } else {
            auto fill = Brush(wizard, p.recess);
            auto edge = Brush(wizard, hovered ? p.ink3 : WithAlpha(p.ink3, 0.7f));
            t->FillRoundedRectangle(Round(box, 5.0f), fill.Get());
            t->DrawRoundedRectangle(Round(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), 4.5f),
                                    edge.Get(), 1.0f);
        }
        DrawText(wizard, control.label, wizard.body.Get(), D2D1::RectF(r.left + 30.0f, r.top + 2.0f, r.right, r.bottom), p.ink);
        break;
    }
    case Kind::Toggle: {
        auto track = Brush(wizard, control.checked ? wizard.accent : p.recess);
        t->FillRoundedRectangle(Round(r, 14.0f), track.Get());
        if (!control.checked) {
            auto edge = Brush(wizard, p.hairline);
            t->DrawRoundedRectangle(Round(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 13.5f),
                                    edge.Get(), 1.0f);
        }
        const float x = control.checked ? r.right - 25.0f : r.left + 3.0f;
        auto knob = Brush(wizard, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        auto shadow = Brush(wizard, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f));
        t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 11.0f, r.top + 15.0f), 11.0f, 11.0f), shadow.Get());
        t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 11.0f, r.top + 14.0f), 11.0f, 11.0f), knob.Get());
        break;
    }
    }
    if (focused) {
        auto ring = Brush(wizard, wizard.accent);
        const auto ringRect = control.kind == Kind::Checkbox
                                  ? D2D1::RectF(r.left - 4.0f, r.top - 1.0f, r.right + 4.0f, r.bottom + 1.0f)
                                  : D2D1::RectF(r.left - 3.0f, r.top - 3.0f, r.right + 3.0f, r.bottom + 3.0f);
        const float radius = control.kind == Kind::Checkbox || control.kind == Kind::Link ? 8.0f
                                                                                          : (ringRect.bottom - ringRect.top) / 2.0f;
        t->DrawRoundedRectangle(Round(ringRect, radius), ring.Get(), 2.0f);
    }
}

void DrawLogo(const Wizard& wizard, float x, float y, float size) {
    if (wizard.icon) {
        wizard.target->DrawBitmap(wizard.icon.Get(), D2D1::RectF(x, y, x + size, y + size), 1.0f,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

void DrawWordmark(const Wizard& wizard, float x, float y) {
    Com<ID2D1DeviceContext5> context;
    if (wizard.wordmark && SUCCEEDED(wizard.target->QueryInterface(IID_PPV_ARGS(context.Put())))) {
        D2D1_MATRIX_3X2_F saved{};
        context->GetTransform(&saved);
        context->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * saved);
        context->DrawSvgDocument(wizard.wordmark.Get());
        context->SetTransform(saved);
        return;
    }
    DrawText(wizard, L"TEKITO", wizard.title.Get(), D2D1::RectF(x, y - 4.0f, x + 200.0f, y + 32.0f), Colors(wizard).ink);
}

void Paint(Wizard& wizard) {
    if (!EnsureTarget(wizard)) return;
    const auto& p = Colors(wizard);
    ID2D1RenderTarget* t = wizard.target.Get();
    t->BeginDraw();
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    t->Clear(wizard.mica ? D2D1::ColorF(0.0f, 0.0f) : p.background);

    const float bodyWidth = kWidth - 2.0f * kMargin;
    switch (wizard.screen) {
    case Screen::Welcome:
        DrawLogo(wizard, kMargin, 42.0f, 60.0f);
        DrawWordmark(wizard, kMargin + 78.0f, 58.0f);
        DrawText(wizard, Tr(L"English and Japanese input for Windows", L"英語と日本語を打つための入力方式"), wizard.title.Get(),
                 D2D1::RectF(kMargin, 128.0f, kWidth - kMargin, 164.0f), p.ink);
        DrawText(wizard,
                 Tr(L"TEKITO fixes English typos as you type, and turns romaji into Japanese. Everything runs on "
                    L"this PC, and nothing you type is ever sent anywhere.",
                    L"TEKITO は英語の打ち間違いを直し、ローマ字を日本語にします。処理はすべてこの PC の中で行われ、"
                    L"入力した内容がどこかに送られることはありません。"),
                 wizard.body.Get(), D2D1::RectF(kMargin, 170.0f, kMargin + bodyWidth, 214.0f), p.ink2);
        if (wizard.webViewReady) {
            DrawText(wizard,
                     Tr(L"Downloads the Japanese dictionary and language data (about 48 MB) from TEKITO's release "
                        L"on GitHub while installing.",
                        L"インストール中に、日本語の辞書と言語データ（約 48 MB）を GitHub の TEKITO のリリースから"
                        L"ダウンロードします。"),
                     wizard.caption.Get(),
                     D2D1::RectF(kMargin + 30.0f, JapaneseRow(wizard) + 28.0f, kWidth - kMargin, JapaneseRow(wizard) + 64.0f),
                     p.ink2);
        }
        if (!wizard.webViewReady) {
            DrawCard(wizard, kWebViewCard);
            DrawText(wizard, Tr(L"One more thing is needed first", L"先にもうひとつ必要なものがあります"), wizard.body.Get(),
                     D2D1::RectF(kWebViewCard.left + 18.0f, kWebViewCard.top + 14.0f, kWebViewCard.right - 18.0f,
                                 kWebViewCard.top + 34.0f),
                     p.problem);
            DrawText(wizard, Tr(L"TEKITO Settings needs the Microsoft Edge WebView2 Runtime.", L"TEKITO の設定画面には Microsoft Edge WebView2 Runtime が必要です。"),
                     wizard.caption.Get(),
                     D2D1::RectF(kWebViewCard.left + 18.0f, kWebViewCard.top + 36.0f, kWebViewCard.right - 18.0f,
                                 kWebViewCard.top + 70.0f),
                     p.ink2);
        }
        break;
    case Screen::License: {
        DrawText(wizard, Tr(L"License terms", L"使用許諾契約"), wizard.heading.Get(), D2D1::RectF(kMargin, 34.0f, kWidth - kMargin, 64.0f), p.ink);
        DrawText(wizard, Tr(L"Please read these before installing TEKITO.", L"インストールの前にお読みください（英文）。"), wizard.caption.Get(),
                 D2D1::RectF(kMargin, 68.0f, kWidth - kMargin, 88.0f), p.ink2);
        DrawCard(wizard, kLicenseCard);
        if (wizard.license) {
            const float viewTop = kLicenseCard.top + 18.0f;
            const float viewHeight = (kLicenseCard.bottom - kLicenseCard.top) - 36.0f;
            DWRITE_TEXT_METRICS metrics{};
            wizard.license->GetMetrics(&metrics);
            const float maxScroll = std::max(0.0f, metrics.height - viewHeight);
            wizard.licenseScroll = std::clamp(wizard.licenseScroll, 0.0f, maxScroll);
            t->PushAxisAlignedClip(D2D1::RectF(kLicenseCard.left + 1.0f, viewTop, kLicenseCard.right - 1.0f, viewTop + viewHeight),
                                   D2D1_ANTIALIAS_MODE_ALIASED);
            auto ink = Brush(wizard, p.ink2);
            t->DrawTextLayout(D2D1::Point2F(kLicenseCard.left + 22.0f, viewTop - wizard.licenseScroll), wizard.license.Get(),
                              ink.Get());
            t->PopAxisAlignedClip();
            if (maxScroll > 0.0f) {
                const float trackTop = viewTop;
                const float trackHeight = viewHeight;
                const float thumbHeight = std::max(28.0f, trackHeight * viewHeight / metrics.height);
                const float thumbTop = trackTop + (trackHeight - thumbHeight) * (wizard.licenseScroll / maxScroll);
                auto thumb = Brush(wizard, WithAlpha(p.ink3, 0.6f));
                t->FillRoundedRectangle(Round(D2D1::RectF(kLicenseCard.right - 10.0f, thumbTop, kLicenseCard.right - 7.0f,
                                                          thumbTop + thumbHeight), 1.5f),
                                        thumb.Get());
            }
        }
        break;
    }
    case Screen::Options: {
        DrawText(wizard, Tr(L"Install options", L"インストールのオプション"), wizard.heading.Get(), D2D1::RectF(kMargin, 34.0f, kWidth - kMargin, 64.0f), p.ink);
        DrawCard(wizard, wizard.japanese ? kOptionsCardJapanese : kOptionsCard);
        const float left = kOptionsCard.left + 18.0f;
        DrawText(wizard, Tr(L"Install location", L"インストール先"), wizard.body.Get(),
                 D2D1::RectF(left, kOptionsCard.top + 16.0f, kOptionsCard.right - 130.0f, kOptionsCard.top + 36.0f), p.ink);
        DrawText(wizard, wizard.installRoot.wstring(), wizard.mono.Get(),
                 D2D1::RectF(left, kOptionsCard.top + 40.0f, kOptionsCard.right - 132.0f, kOptionsCard.top + 58.0f), p.ink2);
        auto divider = Brush(wizard, p.hairline);
        t->DrawLine(D2D1::Point2F(left, kOptionsCard.top + 80.0f), D2D1::Point2F(kOptionsCard.right - 18.0f, kOptionsCard.top + 80.0f),
                    divider.Get(), 1.0f);
        DrawText(wizard, Tr(L"Start menu shortcut", L"スタートメニューのショートカット"), wizard.body.Get(),
                 D2D1::RectF(left, kOptionsCard.top + 98.0f, kOptionsCard.right - 90.0f, kOptionsCard.top + 118.0f), p.ink);
        DrawText(wizard, Tr(L"Adds TEKITO Settings to the Start menu.", L"TEKITO の設定をスタートメニューに追加します。"), wizard.caption.Get(),
                 D2D1::RectF(left, kOptionsCard.top + 120.0f, kOptionsCard.right - 90.0f, kOptionsCard.top + 140.0f), p.ink2);
        if (wizard.japanese) {
            t->DrawLine(D2D1::Point2F(left, kOptionsCard.top + 150.0f),
                        D2D1::Point2F(kOptionsCard.right - 18.0f, kOptionsCard.top + 150.0f), divider.Get(), 1.0f);
            DrawText(wizard, Tr(L"Keep TEKITO for English keyboards", L"英語のキーボードにも TEKITO を残す"), wizard.body.Get(),
                     D2D1::RectF(left, kOptionsCard.top + 160.0f, kOptionsCard.right - 90.0f, kOptionsCard.top + 180.0f), p.ink);
            DrawText(wizard,
                     Tr(L"TEKITO for Japanese types English too, so this is rarely needed.",
                        L"日本語の TEKITO でも英語を打てるので、ふつうは不要です。"),
                     wizard.caption.Get(),
                     D2D1::RectF(left, kOptionsCard.top + 182.0f, kOptionsCard.right - 90.0f, kOptionsCard.top + 212.0f), p.ink2);
        }
        break;
    }
    case Screen::Installing: {
        DrawLogo(wizard, kWidth / 2.0f - 30.0f, 96.0f, 60.0f);
        DrawText(wizard, Tr(L"Installing TEKITO", L"TEKITO をインストールしています"), wizard.headingCentered.Get(), D2D1::RectF(0.0f, 180.0f, kWidth, 212.0f), p.ink);
        DrawText(wizard,
                 wizard.japanese
                     ? Tr(L"Downloading Japanese takes a few minutes. Please keep this window open.",
                          L"日本語のダウンロードに数分かかります。このウィンドウは開いたままにしてください。")
                     : Tr(L"This takes about a minute. Please keep this window open.",
                          L"1 分ほどかかります。このウィンドウは開いたままにしてください。"),
                 wizard.captionCentered.Get(), D2D1::RectF(0.0f, 216.0f, kWidth, 236.0f), p.ink2);
        // Indeterminate progress: a short segment of accent light sliding
        // along a recessed track.
        const auto track = D2D1::RectF(170.0f, 268.0f, 430.0f, 274.0f);
        auto recess = Brush(wizard, p.recess);
        t->FillRoundedRectangle(Round(track, 3.0f), recess.Get());
        const float span = track.right - track.left;
        const float segment = 84.0f;
        const float phase = std::fmod(wizard.animation, 1.0f);
        const float eased = phase < 0.5f ? 2.0f * phase * phase : 1.0f - std::pow(-2.0f * phase + 2.0f, 2.0f) / 2.0f;
        const float left = track.left - segment + (span + segment) * eased;
        t->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto glow = Brush(wizard, WithAlpha(wizard.accent, 0.25f));
        t->FillRoundedRectangle(Round(D2D1::RectF(left - 6.0f, track.top - 3.0f, left + segment + 6.0f, track.bottom + 3.0f), 6.0f),
                                glow.Get());
        auto light = Brush(wizard, wizard.accent);
        t->FillRoundedRectangle(Round(D2D1::RectF(left, track.top, left + segment, track.bottom), 3.0f), light.Get());
        t->PopAxisAlignedClip();
        break;
    }
    case Screen::Done: {
        // A check lit by the accent color, glowing up through the surface.
        const auto center = D2D1::Point2F(kWidth / 2.0f, 138.0f);
        Com<ID2D1GradientStopCollection> stops;
        Com<ID2D1RadialGradientBrush> glow;
        const D2D1_GRADIENT_STOP gradient[] = {{0.0f, WithAlpha(wizard.accent, 0.45f)}, {1.0f, WithAlpha(wizard.accent, 0.0f)}};
        if (SUCCEEDED(t->CreateGradientStopCollection(gradient, 2, stops.Put())) &&
            SUCCEEDED(t->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center, D2D1::Point2F(), 80.0f, 80.0f),
                                                   stops.Get(), glow.Put()))) {
            t->FillEllipse(D2D1::Ellipse(center, 80.0f, 80.0f), glow.Get());
        }
        auto disc = Brush(wizard, wizard.accent);
        t->FillEllipse(D2D1::Ellipse(center, 32.0f, 32.0f), disc.Get());
        auto mark = Brush(wizard, D2D1::ColorF(1.0f, 1.0f, 1.0f));
        Com<ID2D1PathGeometry> path;
        Com<ID2D1GeometrySink> sink;
        Com<ID2D1StrokeStyle> round;
        wizard.factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                      D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                          nullptr, 0, round.Put());
        if (SUCCEEDED(wizard.factory->CreatePathGeometry(path.Put())) && SUCCEEDED(path->Open(sink.Put()))) {
            sink->BeginFigure(D2D1::Point2F(center.x - 12.0f, center.y + 1.0f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(center.x - 3.5f, center.y + 9.5f));
            sink->AddLine(D2D1::Point2F(center.x + 13.0f, center.y - 8.0f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            t->DrawGeometry(path.Get(), mark.Get(), 4.0f, round.Get());
        }
        DrawText(wizard, Tr(L"TEKITO is installed", L"TEKITO をインストールしました"), wizard.headingCentered.Get(), D2D1::RectF(0.0f, 196.0f, kWidth, 228.0f), p.ink);
        DrawText(wizard,
                 Tr(L"Restart the apps you want to type in, then choose TEKITO from the input menu on the "
                    L"taskbar or press Win+Space.",
                    L"使いたいアプリを再起動してから、タスクバーの入力メニューで TEKITO を選ぶか、"
                    L"Win+Space を押してください。"),
                 wizard.bodyCentered.Get(), D2D1::RectF(90.0f, 238.0f, kWidth - 90.0f, 290.0f), p.ink2);
        break;
    }
    case Screen::Failed:
        DrawText(wizard, Tr(L"Setup couldn't finish", L"インストールを完了できませんでした"), wizard.heading.Get(), D2D1::RectF(kMargin, 40.0f, kWidth - kMargin, 72.0f), p.ink);
        DrawText(wizard, FailureMessage(wizard.installExitCode.load()), wizard.body.Get(),
                 D2D1::RectF(kMargin, 84.0f, kWidth - kMargin, 200.0f), p.ink2);
        break;
    }

    const auto controls = Controls(wizard);
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const bool focused = wizard.keyboardFocus && static_cast<int>(i) == wizard.focus;
        DrawControl(wizard, controls[i], static_cast<int>(i) == wizard.hover && controls[i].enabled, focused);
    }

    if (t->EndDraw() == D2DERR_RECREATE_TARGET) ReleaseDeviceResources(wizard);
}

// ---------------------------------------------------------------------------
// Interaction.

void Go(Wizard& wizard, Screen screen) {
    wizard.screen = screen;
    wizard.hover = -1;
    wizard.focus = 0;
    wizard.licenseScroll = 0.0f;
    // Land keyboard focus on the main button of the new screen.
    const auto controls = Controls(wizard);
    for (std::size_t i = 0; i < controls.size(); ++i) {
        if (controls[i].kind == Kind::Primary) wizard.focus = static_cast<int>(i);
    }
    InvalidateRect(wizard.window, nullptr, FALSE);
}

void Activate(Wizard& wizard, Action action) {
    switch (action) {
    case Action::Install:
        if (wizard.agreed && wizard.webViewReady) StartInstall(wizard);
        break;
    case Action::Cancel:
    case Action::Close:
        DestroyWindow(wizard.window);
        return;
    case Action::Options: Go(wizard, Screen::Options); return;
    case Action::ReadLicense: Go(wizard, Screen::License); return;
    case Action::Agree: wizard.agreed = !wizard.agreed; break;
    case Action::GetRuntime: OpenWebView2DownloadPage(); break;
    case Action::CheckRuntime: wizard.webViewReady = HasWebView2Runtime(); break;
    case Action::Back: Go(wizard, Screen::Welcome); return;
    case Action::AcceptLicense:
        wizard.agreed = true;
        Go(wizard, Screen::Welcome);
        return;
    case Action::Browse: {
        const auto selected = BrowseForDirectory(wizard.window);
        if (!selected.empty()) wizard.installRoot = selected / L"TEKITO";
        break;
    }
    case Action::Shortcut: wizard.startMenuShortcut = !wizard.startMenuShortcut; break;
    case Action::Japanese: wizard.japanese = !wizard.japanese; break;
    case Action::KeepEnglish: wizard.keepEnglish = !wizard.keepEnglish; break;
    case Action::Finish:
        wizard.completed = true;
        DestroyWindow(wizard.window);
        return;
    case Action::Retry: StartInstall(wizard); break;
    case Action::WithoutJapanese:
        wizard.japanese = false;
        StartInstall(wizard);
        break;
    }
    InvalidateRect(wizard.window, nullptr, FALSE);
}

D2D1_POINT_2F ToDips(const Wizard& wizard, LPARAM lParam) {
    const float scale = 96.0f / static_cast<float>(wizard.dpi);
    return D2D1::Point2F(GET_X_LPARAM(lParam) * scale, GET_Y_LPARAM(lParam) * scale);
}

int HitTest(const Wizard& wizard, D2D1_POINT_2F point) {
    const auto controls = Controls(wizard);
    for (std::size_t i = 0; i < controls.size(); ++i) {
        const auto& r = controls[i].rect;
        if (point.x >= r.left && point.x <= r.right && point.y >= r.top && point.y <= r.bottom) return static_cast<int>(i);
    }
    return -1;
}

void ApplyTheme(Wizard& wizard) {
    wizard.dark = AppsUseDarkTheme();
    wizard.accent = AccentColor();
    const BOOL dark = wizard.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(wizard.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // Mica behind the whole window; content is drawn on a transparent surface.
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    wizard.mica = SUCCEEDED(DwmSetWindowAttribute(wizard.window, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
    if (wizard.mica) {
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(wizard.window, &margins);
    }
}

void SizeWindow(const Wizard& wizard) {
    RECT rect{0, 0, MulDiv(static_cast<int>(kWidth), static_cast<int>(wizard.dpi), 96),
              MulDiv(static_cast<int>(kHeight), static_cast<int>(wizard.dpi), 96)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(wizard.window, GWL_STYLE));
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, wizard.dpi);
    SetWindowPos(wizard.window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK WizardProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* wizard = reinterpret_cast<Wizard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        wizard = static_cast<Wizard*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        wizard->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(wizard));
    }
    if (!wizard) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        Paint(*wizard);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wizard->target) wizard->target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        wizard->dpi = HIWORD(wParam);
        ReleaseDeviceResources(*wizard);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lParam && std::wstring_view(reinterpret_cast<const wchar_t*>(lParam)) == L"ImmersiveColorSet") {
            ApplyTheme(*wizard);
            ReleaseDeviceResources(*wizard);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_ACTIVATE:
        // Coming back after installing WebView2 should just work.
        if (LOWORD(wParam) != WA_INACTIVE && wizard->screen == Screen::Welcome && !wizard->webViewReady) {
            wizard->webViewReady = HasWebView2Runtime();
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSEMOVE: {
        const int hover = HitTest(*wizard, ToDips(*wizard, lParam));
        if (hover != wizard->hover) {
            wizard->hover = hover;
            SetCursor(LoadCursorW(nullptr, hover >= 0 && Controls(*wizard)[hover].enabled ? IDC_HAND : IDC_ARROW));
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            const bool onControl = wizard->hover >= 0 && Controls(*wizard)[wizard->hover].enabled;
            SetCursor(LoadCursorW(nullptr, onControl ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        wizard->hover = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        wizard->pressed = true;
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (!wizard->pressed) return 0;
        wizard->pressed = false;
        wizard->keyboardFocus = false;
        const int hit = HitTest(*wizard, ToDips(*wizard, lParam));
        const auto controls = Controls(*wizard);
        if (hit >= 0 && controls[hit].enabled) {
            wizard->focus = hit;
            Activate(*wizard, controls[hit].action);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (wizard->screen == Screen::License) {
            wizard->licenseScroll -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * 57.0f;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN: {
        const auto controls = Controls(*wizard);
        if (wParam == VK_TAB && !controls.empty()) {
            const int direction = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
            const int count = static_cast<int>(controls.size());
            int next = wizard->focus;
            for (int step = 0; step < count; ++step) {
                next = (next + direction + count) % count;
                if (controls[next].enabled) break;
            }
            wizard->focus = next;
            wizard->keyboardFocus = true;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if ((wParam == VK_RETURN || wParam == VK_SPACE) && !controls.empty()) {
            const int index = std::clamp(wizard->focus, 0, static_cast<int>(controls.size()) - 1);
            if (controls[index].enabled) Activate(*wizard, controls[index].action);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (wizard->screen == Screen::License || wizard->screen == Screen::Options) Go(*wizard, Screen::Welcome);
            else if (wizard->screen != Screen::Installing) DestroyWindow(window);
            return 0;
        }
        if (wizard->screen == Screen::License) {
            const float step = wParam == VK_UP ? -38.0f : wParam == VK_DOWN ? 38.0f
                             : wParam == VK_PRIOR ? -240.0f : wParam == VK_NEXT ? 240.0f
                             : wParam == VK_HOME ? -1e6f : wParam == VK_END ? 1e6f : 0.0f;
            if (step != 0.0f) {
                wizard->licenseScroll += step;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        break;
    }
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            wizard->animation += 0.016f / 1.4f;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case kWizardInstallComplete:
        if (wizard->installThread.joinable()) wizard->installThread.join();
        KillTimer(window, kAnimationTimer);
        Go(*wizard, wizard->installExitCode.load() == 0 ? Screen::Done : Screen::Failed);
        return 0;
    case WM_CLOSE:
        if (wizard->screen == Screen::Installing) return 0;  // let the install finish
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

#ifdef TEKITO_SETUP_PREVIEW
// Preview build (tekito_setup_preview): opens one screen without elevation,
// saves a screenshot of the window and exits.
//   tekito_setup_preview.exe <screen> <out.bmp> [nowebview] [unchecked] [nojapanese] [nodownload]
// screen: welcome | license | options | installing | done | failed
struct PreviewRequest {
    Screen screen{Screen::Welcome};
    std::wstring output;
    bool noWebView{false};
    bool agreed{true};
    bool noJapanese{false};
    // The failed screen after a Japanese download failure.
    bool noDownload{false};
};
PreviewRequest g_preview;

void SavePreview(HWND window, const std::wstring& path) {
    RECT rect{};
    DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    BITMAPINFOHEADER header{sizeof(header), width, -height, 1, 32, BI_RGB};
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    BITMAPINFO info{};
    info.bmiHeader = header;
    GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}
#endif

bool ShowWizard(const std::filesystem::path& self) {
    Wizard wizard;
    wizard.self = self;
    wizard.installRoot = DefaultInstallRoot();
    wizard.webViewReady = HasWebView2Runtime();
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, wizard.factory.Put())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(wizard.dwrite.Put())))) {
        MessageBoxW(nullptr, Tr(L"TEKITO Setup couldn't start its graphics.", L"セットアップの画面を表示できませんでした。"), Tr(L"TEKITO Setup", L"TEKITO セットアップ"), MB_OK | MB_ICONERROR);
        return false;
    }
    LoadLicenseText(wizard);
    LoadBrandFont(wizard);
    CreateTextResources(wizard);

    const wchar_t className[] = L"TekitoSetupWizard";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WizardProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(kLogoIcon));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = className;
    RegisterClassExW(&windowClass);

    const HWND window = CreateWindowExW(0, className, Tr(L"TEKITO Setup", L"TEKITO セットアップ"), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, windowClass.hInstance,
                                        &wizard);
    if (!window) return false;
    wizard.dpi = GetDpiForWindow(window);
    ApplyTheme(wizard);
    SizeWindow(wizard);
    RECT rect{};
    GetWindowRect(window, &rect);
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    SetWindowPos(window, nullptr, info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2,
                 info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    Go(wizard, Screen::Welcome);
#ifdef TEKITO_SETUP_PREVIEW
    wizard.webViewReady = !g_preview.noWebView;
    wizard.agreed = g_preview.agreed;
    if (g_preview.noJapanese) wizard.japanese = false;
    if (g_preview.screen == Screen::Installing) {
        StartInstall(wizard);
        wizard.animation = 0.35f;
    } else {
        if (g_preview.screen == Screen::Failed) wizard.installExitCode.store(g_preview.noDownload ? 20 : 4);
        Go(wizard, g_preview.screen);
    }
#endif
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
#ifdef TEKITO_SETUP_PREVIEW
    SetForegroundWindow(window);
    const auto started = GetTickCount64();
    MSG pending{};
    while (GetTickCount64() - started < 1500) {
        while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pending);
            DispatchMessageW(&pending);
        }
        Sleep(10);
    }
    KillTimer(window, kAnimationTimer);
    wizard.animation = 0.35f;
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
    Sleep(200);
    SavePreview(window, g_preview.output);
    wizard.screen = Screen::Done;  // allow closing
    DestroyWindow(window);
#endif

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (wizard.installThread.joinable()) wizard.installThread.join();
    ReleaseDeviceResources(wizard);
    return wizard.completed;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#ifdef TEKITO_SETUP_PREVIEW
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3) {
        const std::wstring_view screen = argv[1];
        g_preview.screen = screen == L"license" ? Screen::License
                         : screen == L"options" ? Screen::Options
                         : screen == L"installing" ? Screen::Installing
                         : screen == L"done" ? Screen::Done
                         : screen == L"failed" ? Screen::Failed
                                               : Screen::Welcome;
        g_preview.output = argv[2];
        for (int i = 3; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"nowebview") g_preview.noWebView = true;
            if (std::wstring_view(argv[i]) == L"unchecked") g_preview.agreed = false;
            if (std::wstring_view(argv[i]) == L"nojapanese") g_preview.noJapanese = true;
            if (std::wstring_view(argv[i]) == L"nodownload") g_preview.noDownload = true;
        }
    }
    LocalFree(argv);
#endif
    wchar_t selfPath[MAX_PATH * 4]{};
    const auto length = GetModuleFileNameW(nullptr, selfPath, static_cast<DWORD>(std::size(selfPath)));
    if (length == 0 || length >= std::size(selfPath)) return 1;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialized = SUCCEEDED(comResult);
    const int result = ShowWizard(std::filesystem::path(selfPath)) ? 0 : 1;
    if (comInitialized) CoUninitialize();
    return result;
}
