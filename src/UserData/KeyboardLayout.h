#pragma once

#include <windows.h>

namespace tekito::userdata {

// Whether the physical keyboard is a Japanese (JIS, 106/109 key) one, from
// UserSettings::keyboardType, asking Windows when it is 0.
inline bool UsesJapaneseKeyboard(int keyboardType) noexcept {
    if (keyboardType == 1) return true;
    if (keyboardType == 2) return false;
    return GetKeyboardType(0) == 7;  // 7 = Japanese keyboard
}

// The layout the English profile types with. On a Japanese keyboard it is
// the Japanese layout, so every symbol comes out as printed on the key.
inline HKL EnglishProfileLayout(int keyboardType) noexcept {
    return UsesJapaneseKeyboard(keyboardType)
               ? reinterpret_cast<HKL>(static_cast<ULONG_PTR>(0x04110411))
               : nullptr;
}

}  // namespace tekito::userdata
