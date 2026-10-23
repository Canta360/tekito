// TEKITO online installer: a small stub that downloads the latest full
// installer from GitHub Releases, checks its SHA-256, then runs it.
// Optional /enable-updates is passed through when the user asks to check
// for updates later. The input method itself never uses the network.
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr wchar_t kRepoHost[] = L"api.github.com";
constexpr wchar_t kRepoPath[] = L"/repos/Canta360/tekito/releases/latest";
constexpr wchar_t kUserAgent[] = L"TEKITO-OnlineInstaller";

bool UseJapanese() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
}

const wchar_t* Tr(const wchar_t* english, const wchar_t* japanese) {
    return UseJapanese() ? japanese : english;
}

std::filesystem::path MakeTempDirectory() {
    wchar_t tempPath[MAX_PATH]{};
    const auto length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) return {};
    for (unsigned int attempt = 0; attempt < 10; ++attempt) {
        const auto candidate = std::filesystem::path(tempPath) /
                               (L"TEKITO-Online-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error) && !error) return candidate;
    }
    return {};
}

bool HttpGet(const wchar_t* host, INTERNET_PORT port, const wchar_t* path, bool https,
             std::vector<char>& body, std::wstring& error) {
    body.clear();
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = Tr(L"Could not start a download.", L"ダウンロードを開始できませんでした。");
        return false;
    }
    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        error = Tr(L"Could not reach the download server.", L"ダウンロード先に接続できませんでした。");
        return false;
    }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = Tr(L"Could not start a download.", L"ダウンロードを開始できませんでした。");
        return false;
    }
    // GitHub requires a User-Agent; Accept helps get JSON without extra noise.
    WinHttpAddRequestHeaders(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = Tr(L"The download failed. Check the internet connection and try again.",
                   L"ダウンロードに失敗しました。インターネット接続を確認してもう一度お試しください。");
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status,
                        &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        error = Tr(L"The download server returned an error. Try again later.",
                   L"ダウンロード先がエラーを返しました。しばらくしてからもう一度お試しください。");
        return false;
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) break;
        if (available == 0) break;
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &read)) {
            body.resize(offset);
            break;
        }
        body.resize(offset + read);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return !body.empty();
}

bool DownloadUrlToFile(const std::wstring& url, const std::filesystem::path& dest, std::wstring& error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || !parts.lpszHostName || !parts.lpszUrlPath) {
        error = Tr(L"The download address is invalid.", L"ダウンロード先のアドレスが正しくありません。");
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    std::vector<char> body;
    if (!HttpGet(host.c_str(), parts.nPort, path.c_str(), https, body, error)) return false;
    std::ofstream output(dest, std::ios::binary);
    if (!output.write(body.data(), static_cast<std::streamsize>(body.size()))) {
        error = Tr(L"Could not save the downloaded file.", L"ダウンロードしたファイルを保存できませんでした。");
        return false;
    }
    return true;
}

std::string Sha256Hex(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectLength = 0;
    DWORD dataLength = 0;
    DWORD hashLength = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &dataLength, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
                          &dataLength, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<UCHAR> object(objectLength);
    std::vector<UCHAR> digest(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<char> buffer(1024 * 256);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = input.gcount();
        if (got > 0 &&
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(got), 0) != 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return {};
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hashLength, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string hex;
    hex.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = kHex[digest[i] >> 4];
        hex[i * 2 + 1] = kHex[digest[i] & 0xf];
    }
    return hex;
}

std::string ExpectedSha256(const std::filesystem::path& shaFile) {
    std::ifstream input(shaFile);
    if (!input) return {};
    std::string line;
    std::getline(input, line);
    // "HASH  filename" or "HASH"
    const auto end = line.find_first_of(" \t");
    std::string hash = end == std::string::npos ? line : line.substr(0, end);
    for (char& c : hash) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return hash;
}

// Finds the browser_download_url for TEKITO-*-full-installer.exe in a GitHub
// release JSON body (no nested JSON parser: scan for the asset name pattern).
bool FindFullInstallerUrl(const std::vector<char>& json, std::wstring& url, std::wstring& name,
                          std::wstring& error) {
    const std::string body(json.begin(), json.end());
    const std::string marker = "TEKITO-";
    const std::string suffix = "-full-installer.exe";
    std::size_t search = 0;
    while (true) {
        const auto namePos = body.find(marker, search);
        if (namePos == std::string::npos) break;
        const auto exePos = body.find(suffix, namePos);
        if (exePos == std::string::npos || exePos > namePos + 64) {
            search = namePos + marker.size();
            continue;
        }
        const auto nameEnd = exePos + suffix.size();
        // Skip the .sha256 sidecar entry.
        if (nameEnd < body.size() && body.compare(nameEnd, 7, ".sha256") == 0) {
            search = nameEnd;
            continue;
        }
        const std::string assetName = body.substr(namePos, nameEnd - namePos);
        // GitHub lists name, then browser_download_url, in each asset object.
        const auto urlKey = body.find("\"browser_download_url\"", namePos);
        if (urlKey == std::string::npos || urlKey > nameEnd + 400) {
            search = nameEnd;
            continue;
        }
        const auto colon = body.find(':', urlKey);
        const auto quote1 = body.find('"', colon);
        const auto quote2 = body.find('"', quote1 + 1);
        if (quote1 == std::string::npos || quote2 == std::string::npos) {
            search = nameEnd;
            continue;
        }
        const std::string asciiUrl = body.substr(quote1 + 1, quote2 - quote1 - 1);
        if (asciiUrl.find(assetName) == std::string::npos) {
            search = nameEnd;
            continue;
        }
        url.assign(asciiUrl.begin(), asciiUrl.end());
        name.assign(assetName.begin(), assetName.end());
        return true;
    }
    error = Tr(L"The latest release has no full installer. Try again later.",
               L"最新のリリースにフルインストーラーがありません。しばらくしてからもう一度お試しください。");
    return false;
}

struct DownloadResult {
    std::filesystem::path installer;
    std::wstring error;
    bool ok{false};
};

DownloadResult DownloadLatestInstaller() {
    DownloadResult result;
    std::vector<char> releaseJson;
    if (!HttpGet(kRepoHost, INTERNET_DEFAULT_HTTPS_PORT, kRepoPath, true, releaseJson, result.error)) {
        return result;
    }
    std::wstring url;
    std::wstring name;
    if (!FindFullInstallerUrl(releaseJson, url, name, result.error)) return result;
    const auto temp = MakeTempDirectory();
    if (temp.empty()) {
        result.error = Tr(L"Could not create a temporary folder.", L"一時フォルダーを作成できませんでした。");
        return result;
    }
    result.installer = temp / name;
    const auto shaPath = temp / (name + L".sha256");
    if (!DownloadUrlToFile(url, result.installer, result.error)) return result;
    if (!DownloadUrlToFile(url + L".sha256", shaPath, result.error)) {
        // Sidecar may be listed as a separate asset; try constructing from
        // the same directory name pattern used on the release page.
        result.error.clear();
        std::wstring shaUrl = url;
        const auto slash = shaUrl.find_last_of(L'/');
        if (slash != std::wstring::npos) {
            shaUrl = shaUrl.substr(0, slash + 1) + name + L".sha256";
            if (!DownloadUrlToFile(shaUrl, shaPath, result.error)) return result;
        } else {
            return result;
        }
    }
    const auto expected = ExpectedSha256(shaPath);
    const auto actual = Sha256Hex(result.installer);
    if (expected.empty() || actual.empty() || expected != actual) {
        result.error = Tr(L"The downloaded installer did not match its checksum. Try again.",
                          L"ダウンロードしたインストーラーのチェックサムが一致しません。もう一度お試しください。");
        return result;
    }
    result.ok = true;
    return result;
}

INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowTextW(window, Tr(L"TEKITO Setup", L"TEKITO セットアップ"));
        SetDlgItemTextW(window, 1001,
                        Tr(L"This downloads the latest TEKITO installer and runs it. English is included; "
                           L"Japanese data can be added in setup or later.",
                           L"最新の TEKITO インストーラーをダウンロードして実行します。英語は含まれ、"
                           L"日本語データはセットアップ中またはあとから追加できます。"));
        SetDlgItemTextW(window, 1002,
                        Tr(L"Check for updates after installing", L"インストール後に更新を確認する"));
        SetDlgItemTextW(window, IDOK, Tr(L"Download and install", L"ダウンロードしてインストール"));
        SetDlgItemTextW(window, IDCANCEL, Tr(L"Cancel", L"キャンセル"));
        SetDlgItemTextW(window, 1003, L"");
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            EnableWindow(GetDlgItem(window, IDOK), FALSE);
            EnableWindow(GetDlgItem(window, IDCANCEL), FALSE);
            EnableWindow(GetDlgItem(window, 1002), FALSE);
            SetDlgItemTextW(window, 1003, Tr(L"Downloading the latest installer…", L"最新のインストーラーをダウンロードしています…"));
            // Run the download on this UI thread: the stub is short-lived and
            // a modal wait is fine; pumping messages keeps the window alive.
            MSG pump{};
            while (PeekMessageW(&pump, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&pump);
                DispatchMessageW(&pump);
            }
            const bool enableUpdates = IsDlgButtonChecked(window, 1002) == BST_CHECKED;
            const auto downloaded = DownloadLatestInstaller();
            if (!downloaded.ok) {
                MessageBoxW(window, downloaded.error.c_str(), Tr(L"TEKITO Setup", L"TEKITO セットアップ"),
                            MB_OK | MB_ICONERROR);
                EnableWindow(GetDlgItem(window, IDOK), TRUE);
                EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
                EnableWindow(GetDlgItem(window, 1002), TRUE);
                SetDlgItemTextW(window, 1003, L"");
                return TRUE;
            }
            SetDlgItemTextW(window, 1003, Tr(L"Starting setup…", L"セットアップを起動しています…"));
            std::wstring params = enableUpdates ? L"/enable-updates" : L"";
            const auto launched = reinterpret_cast<INT_PTR>(
                ShellExecuteW(window, L"open", downloaded.installer.c_str(),
                              params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNORMAL));
            if (launched <= 32) {
                MessageBoxW(window,
                            Tr(L"The installer downloaded, but could not be started.",
                               L"インストーラーはダウンロードできましたが、起動できませんでした。"),
                            Tr(L"TEKITO Setup", L"TEKITO セットアップ"), MB_OK | MB_ICONERROR);
                EnableWindow(GetDlgItem(window, IDOK), TRUE);
                EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
                EnableWindow(GetDlgItem(window, 1002), TRUE);
                SetDlgItemTextW(window, 1003, L"");
                return TRUE;
            }
            EndDialog(window, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Build a simple dialog template in memory (avoids a separate .rc for the stub).
LPWORD DwordAlign(LPWORD pointer) {
    const auto address = reinterpret_cast<ULONG_PTR>(pointer);
    return reinterpret_cast<LPWORD>((address + 3) & ~static_cast<ULONG_PTR>(3));
}

void AppendString(LPWORD& pointer, const wchar_t* text) {
    while (*text) *pointer++ = *text++;
    *pointer++ = 0;
}

HGLOBAL BuildDialogTemplate() {
    // DLGTEMPLATE + title + font + controls (static, checkbox, status, OK, Cancel).
    const auto size = 2048;
    HGLOBAL memory = GlobalAlloc(GMEM_ZEROINIT, size);
    if (!memory) return nullptr;
    auto* bytes = static_cast<BYTE*>(GlobalLock(memory));
    auto* dialog = reinterpret_cast<DLGTEMPLATE*>(bytes);
    dialog->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dialog->cdit = 5;
    dialog->x = 0;
    dialog->y = 0;
    dialog->cx = 280;
    dialog->cy = 140;
    LPWORD pointer = reinterpret_cast<LPWORD>(dialog + 1);
    *pointer++ = 0;  // no menu
    *pointer++ = 0;  // default class
    AppendString(pointer, L"TEKITO");
    *pointer++ = 9;  // font size
    AppendString(pointer, L"Segoe UI");
    pointer = DwordAlign(pointer);

    auto addControl = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        pointer = DwordAlign(pointer);
        auto* item = reinterpret_cast<DLGITEMTEMPLATE*>(pointer);
        item->style = style | WS_CHILD | WS_VISIBLE;
        item->dwExtendedStyle = 0;
        item->x = x;
        item->y = y;
        item->cx = cx;
        item->cy = cy;
        item->id = id;
        pointer = reinterpret_cast<LPWORD>(item + 1);
        *pointer++ = 0xffff;
        *pointer++ = cls;
        AppendString(pointer, text);
        *pointer++ = 0;  // no creation data
    };

    addControl(SS_LEFT, 10, 10, 260, 48, 1001, 0x0082, L"");           // STATIC
    addControl(BS_AUTOCHECKBOX | WS_TABSTOP, 10, 64, 260, 14, 1002, 0x0080, L"");  // BUTTON
    addControl(SS_LEFT, 10, 84, 260, 20, 1003, 0x0082, L"");           // status
    addControl(BS_DEFPUSHBUTTON | WS_TABSTOP, 100, 110, 90, 16, IDOK, 0x0080, L"OK");
    addControl(BS_PUSHBUTTON | WS_TABSTOP, 196, 110, 70, 16, IDCANCEL, 0x0080, L"Cancel");

    GlobalUnlock(memory);
    return memory;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HGLOBAL dialog = BuildDialogTemplate();
    if (!dialog) return 1;
    const INT_PTR result = DialogBoxIndirectParamW(instance, static_cast<LPCDLGTEMPLATEW>(GlobalLock(dialog)), nullptr,
                                                   DialogProc, 0);
    GlobalUnlock(dialog);
    GlobalFree(dialog);
    return result == IDOK ? 0 : 1;
}
