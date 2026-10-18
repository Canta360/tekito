#pragma once

#include "Core/Japanese/JapaneseKeys.h"

#include <windows.h>

namespace tekito::tsf {

// A virtual key and the keyboard state now, as the Japanese composer's key
// handling reads it (japanese::TranslateKey). Shared by the text service
// and the testbed. `ignoreCapsLock` types letters as if Caps Lock were off.
// `kanaInput`: the keys type the kana printed on a JIS keyboard (かな入力),
// by where they are, so a US keyboard types them from the same places.
[[nodiscard]] japanese::KeyPress JapaneseKeyPressFor(WPARAM virtualKey, bool ignoreCapsLock, bool kanaInput = false);

}  // namespace tekito::tsf
