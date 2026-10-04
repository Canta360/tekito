#pragma once

#include "Core/Japanese/JapaneseKeys.h"

#include <windows.h>

namespace tekito::tsf {

// A virtual key and the keyboard state now, as the Japanese composer's key
// handling reads it (japanese::TranslateKey). Shared by the text service
// and the testbed. `ignoreCapsLock` types letters as if Caps Lock were off.
[[nodiscard]] japanese::KeyPress JapaneseKeyPressFor(WPARAM virtualKey, bool ignoreCapsLock);

}  // namespace tekito::tsf
