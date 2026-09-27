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
    // Rows in the candidate list: 0 = automatic (English shows 5, then 10
    // once paged; Japanese 9), or a fixed 5, 7 or 9.
    int candidateRows{0};
    // The candidate list shows what the highlighted word means, in a pane
    // beside it (the japanese-wiktionary, japanese-wordnet and
    // dictionary-display packs).
    bool meaningsEnabled{true};
    // Key that switches between Convert and Direct: 0 = none, 1 = Alt+`,
    // 2 = Ctrl+Space, 3 = Ctrl+Shift+Space.
    int toggleKey{1};
    // Enter adds a period to a line that ends without punctuation.
    bool periodOnEnter{false};
    // The physical keyboard: 0 = as Windows reports it, 1 = Japanese (JIS,
    // 106/109 keys), 2 = US (101/102 keys). Decides the switch keys and the
    // layout the English profile uses. See KeyboardLayout.h.
    int keyboardType{0};
    // The ja-JP profile's mode (Japanese, Convert or Direct), and which of
    // Convert and Direct its switch key goes to from Japanese.
    InputMode lastJapaneseProfileMode{InputMode::Japanese};
    InputMode japaneseProfileEnglishMode{InputMode::Convert};
    // Space outside a composition in Japanese mode: 0 = full-width in
    // Japanese and half-width otherwise, 1 = always half-width, 2 = always
    // full-width. Shift+Space writes the other one.
    int japaneseSpaceWidth{0};
    // The marks for the comma and period keys in Japanese: 0 = touten and
    // kuten, 1 = full-width comma and period, 2 = comma and kuten,
    // 3 = touten and period (see japanese::PunctuationStyle).
    int japanesePunctuation{0};
    // Words that start with what is typed, offered while typing Japanese.
    bool japanesePredictionEnabled{true};
    // Language of TEKITO's own windows: 0 = the Windows display language,
    // 1 = English, 2 = Japanese. See UiLanguage.h.
    int uiLanguage{0};
    // Executable names (e.g. "code.exe") where TEKITO passes every key
    // through, in addition to the built-in terminal list.
    std::vector<std::wstring> excludedApps;
};

}  // namespace tekito::userdata
