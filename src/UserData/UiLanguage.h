#pragma once

#include <windows.h>

namespace tekito::userdata {

// Whether TEKITO's own windows (Settings, the candidate list labels, the
// Language Bar menu) should be in Japanese, from UserSettings::uiLanguage.
inline bool UseJapaneseUi(int uiLanguage) noexcept {
    if (uiLanguage == 1) return false;
    if (uiLanguage == 2) return true;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
}

}  // namespace tekito::userdata
