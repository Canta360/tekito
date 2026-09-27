#pragma once

#include "Core/InputMode.h"

#include <string>
#include <vector>

namespace tekito::userdata {

// Terminals send keys straight to a shell, where word correction does more
// harm than good. TEKITO always stays off in these.
inline constexpr const wchar_t* kBuiltInExcludedApps[] = {
    L"WindowsTerminal.exe", L"OpenConsole.exe", L"conhost.exe",
    L"cmd.exe",             L"powershell.exe",  L"pwsh.exe",
};

struct UserSettings {
    InputMode defaultInputMode{InputMode::Convert};
    InputMode lastInputMode{InputMode::Convert};
    bool restoreLastInputMode{true};
    bool learningEnabled{true};
    bool correctionEnabled{true};
    bool commonMisspellingsEnabled{true};
    bool contextSuggestionsEnabled{true};
    bool completionEnabled{true};
    bool japanesePhoneticSuggestionsEnabled{true};
    int socialExpressionRange{1};
    int socialPersonalization{1};
    bool candidateWindowEnabled{true};
    // 0 = glass (frosted, with light under the selection), 1 = simple
    // (opaque panel with a flat selection row).
    int candidateWindowStyle{0};
    // Key that switches between Convert and Direct: 0 = none, 1 = Alt+`,
    // 2 = Ctrl+Space, 3 = Ctrl+Shift+Space.
    int toggleKey{1};
    // Enter adds a period to a line that ends without punctuation.
    bool periodOnEnter{false};
    // Language of TEKITO's own windows: 0 = the Windows display language,
    // 1 = English, 2 = Japanese. See UiLanguage.h.
    int uiLanguage{0};
    // Executable names (e.g. "code.exe") where TEKITO passes every key
    // through, in addition to the built-in terminal list.
    std::vector<std::wstring> excludedApps;
};

}  // namespace tekito::userdata
