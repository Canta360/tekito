#include "Tsf/JapaneseKeyPress.h"

#include <iterator>

namespace tekito::tsf {
namespace {

bool KeyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }

}  // namespace

japanese::KeyPress JapaneseKeyPressFor(WPARAM virtualKey, bool ignoreCapsLock) {
    using Key = japanese::KeyPress::Key;
    japanese::KeyPress press;
    press.shift = KeyDown(VK_SHIFT);
    press.command = KeyDown(VK_CONTROL) || KeyDown(VK_MENU) || KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
    if (virtualKey >= '1' && virtualKey <= '9') press.digit = static_cast<int>(virtualKey - '0');
    if (virtualKey >= VK_NUMPAD1 && virtualKey <= VK_NUMPAD9) press.digit = static_cast<int>(virtualKey - VK_NUMPAD0);
    switch (virtualKey) {
    case VK_SPACE: press.key = Key::Space; return press;
    case VK_CONVERT: press.key = Key::Convert; return press;
    case VK_NONCONVERT: press.key = Key::NonConvert; return press;
    case VK_RETURN: press.key = Key::Enter; return press;
    case VK_BACK: press.key = Key::Backspace; return press;
    case VK_ESCAPE: press.key = Key::Escape; return press;
    case VK_TAB: press.key = Key::Tab; return press;
    case VK_LEFT: press.key = Key::Left; return press;
    case VK_RIGHT: press.key = Key::Right; return press;
    case VK_UP: press.key = Key::Up; return press;
    case VK_DOWN: press.key = Key::Down; return press;
    case VK_HOME: press.key = Key::Home; return press;
    case VK_END: press.key = Key::End; return press;
    case VK_PRIOR: press.key = Key::PageUp; return press;
    case VK_NEXT: press.key = Key::PageDown; return press;
    case VK_DELETE: press.key = Key::Delete; return press;
    case VK_INSERT: press.key = Key::Insert; return press;
    case VK_F6: press.key = Key::F6; return press;
    case VK_F7: press.key = Key::F7; return press;
    case VK_F8: press.key = Key::F8; return press;
    case VK_F9: press.key = Key::F9; return press;
    case VK_F10: press.key = Key::F10; return press;
    default: break;
    }
    if (press.command) return press;
    // What the key types with the keyboard layout and state now.
    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState)) return press;
    if (ignoreCapsLock) keyboardState[VK_CAPITAL] &= static_cast<BYTE>(~1u);
    wchar_t buffer[8]{};
    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    const int count = ToUnicodeEx(static_cast<UINT>(virtualKey), scanCode, keyboardState, buffer,
                                  static_cast<int>(std::size(buffer)), 0, GetKeyboardLayout(0));
    if (count == 1) {
        press.key = Key::Character;
        press.character = buffer[0];
    }
    return press;
}

}  // namespace tekito::tsf
