// tekito_uiless_host: a window that draws the input method's candidate list
// itself, as full-screen games do (TSF UI-less mode). It tells TSF not to
// show the input method's own list and writes what the list holds to
// uiless-host.log beside the executable, for checking the candidate list UI
// element without a game.
#include <msctf.h>
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::wstring g_logPath;
ITfThreadMgr* g_threadManager = nullptr;

void Log(const std::wstring& line) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath.c_str(), L"a, ccs=UTF-8") == 0 && file) {
        fwprintf(file, L"%ls\n", line.c_str());
        fclose(file);
    }
}

void LogList(DWORD id) {
    ITfUIElementMgr* elements = nullptr;
    if (FAILED(g_threadManager->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void**>(&elements)))) return;
    ITfUIElement* element = nullptr;
    if (SUCCEEDED(elements->GetUIElement(id, &element)) && element) {
        ITfCandidateListUIElement* list = nullptr;
        if (SUCCEEDED(element->QueryInterface(IID_ITfCandidateListUIElement, reinterpret_cast<void**>(&list)))) {
            UINT count = 0, selection = 0, page = 0, pages = 0;
            list->GetCount(&count);
            list->GetSelection(&selection);
            list->GetCurrentPage(&page);
            list->GetPageIndex(nullptr, 0, &pages);
            std::wstring line = L"list count=" + std::to_wstring(count) + L" selection=" + std::to_wstring(selection) +
                                L" page=" + std::to_wstring(page) + L"/" + std::to_wstring(pages) + L":";
            for (UINT i = 0; i < count && i < 9; ++i) {
                BSTR text = nullptr;
                if (SUCCEEDED(list->GetString(i, &text)) && text) {
                    line += L" ";
                    line += text;
                    SysFreeString(text);
                }
            }
            Log(line);
            list->Release();
        }
        element->Release();
    }
    elements->Release();
}

class Sink final : public ITfUIElementSink {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfUIElementSink)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD id, BOOL* show) override {
        *show = FALSE;  // drawn here, not by the input method
        Log(L"begin " + std::to_wstring(id));
        LogList(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id) override {
        LogList(id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id) override {
        Log(L"end " + std::to_wstring(id));
        return S_OK;
    }
};

Sink g_sink;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static HWND edit = nullptr;
    switch (message) {
    case WM_CREATE:
        edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 0, 0, 0, 0, hwnd, nullptr,
                               nullptr, nullptr);
        return 0;
    case WM_SIZE:
        MoveWindow(edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    case WM_SETFOCUS:
        SetFocus(edit);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_logPath = path;
    g_logPath = g_logPath.substr(0, g_logPath.find_last_of(L'\\') + 1) + L"uiless-host.log";
    DeleteFileW(g_logPath.c_str());

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    DWORD sinkCookie = TF_INVALID_COOKIE;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                   reinterpret_cast<void**>(&g_threadManager)))) {
        TfClientId client = TF_CLIENTID_NULL;
        g_threadManager->Activate(&client);
        ITfSource* source = nullptr;
        if (SUCCEEDED(g_threadManager->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)))) {
            source->AdviseSink(IID_ITfUIElementSink, &g_sink, &sinkCookie);
            source->Release();
        }
    }
    Log(sinkCookie != TF_INVALID_COOKIE ? L"sink advised" : L"sink not advised");

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"TekitoUilessHost";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"TEKITO UI-less host", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 640, 240, nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_threadManager) {
        ITfSource* source = nullptr;
        if (sinkCookie != TF_INVALID_COOKIE &&
            SUCCEEDED(g_threadManager->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source)))) {
            source->UnadviseSink(sinkCookie);
            source->Release();
        }
        g_threadManager->Deactivate();
        g_threadManager->Release();
    }
    CoUninitialize();
    return 0;
}
