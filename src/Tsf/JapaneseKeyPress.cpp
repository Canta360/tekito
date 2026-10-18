#include "Tsf/JapaneseKeyPress.h"

#include <iterator>
#include <utility>

namespace tekito::tsf {
namespace {

bool KeyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }

// The kana a JIS keyboard has on the key at `scanCode`, with Shift or not;
// 0 for a key without one.
wchar_t KanaAt(UINT scanCode, bool shift) {
    struct KanaKey {
        UINT scan;
        wchar_t plain;
        wchar_t shifted;
    };
    static constexpr KanaKey kKeys[] = {
        {0x02, L'ぬ', 0}, {0x03, L'ふ', 0}, {0x04, L'あ', L'ぁ'}, {0x05, L'う', L'ぅ'}, {0x06, L'え', L'ぇ'},
        {0x07, L'お', L'ぉ'}, {0x08, L'や', L'ゃ'}, {0x09, L'ゆ', L'ゅ'}, {0x0A, L'よ', L'ょ'}, {0x0B, L'わ', L'を'},
        {0x0C, L'ほ', 0}, {0x0D, L'へ', 0}, {0x7D, L'ー', 0},
        {0x10, L'た', 0}, {0x11, L'て', 0}, {0x12, L'い', L'ぃ'}, {0x13, L'す', 0}, {0x14, L'か', 0},
        {0x15, L'ん', 0}, {0x16, L'な', 0}, {0x17, L'に', 0}, {0x18, L'ら', 0}, {0x19, L'せ', 0},
        {0x1A, L'゛', 0}, {0x1B, L'゜', L'「'},
        {0x1E, L'ち', 0}, {0x1F, L'と', 0}, {0x20, L'し', 0}, {0x21, L'は', 0}, {0x22, L'き', 0},
        {0x23, L'く', 0}, {0x24, L'ま', 0}, {0x25, L'の', 0}, {0x26, L'り', 0}, {0x27, L'れ', 0},
        {0x28, L'け', 0}, {0x2B, L'む', L'」'},
        {0x2C, L'つ', L'っ'}, {0x2D, L'さ', 0}, {0x2E, L'そ', 0}, {0x2F, L'ひ', 0}, {0x30, L'こ', 0},
        {0x31, L'み', 0}, {0x32, L'も', 0}, {0x33, L'ね', L'、'}, {0x34, L'る', L'。'}, {0x35, L'め', L'・'},
        {0x73, L'ろ', 0},
    };
    for (const auto& key : kKeys) {
        if (key.scan == scanCode) return shift && key.shifted ? key.shifted : key.plain;
    }
    return 0;
}

}  // namespace

japanese::KeyPress JapaneseKeyPressFor(WPARAM virtualKey, bool ignoreCapsLock, bool kanaInput) {
    using Key = japanese::KeyPress::Key;
    japanese::KeyPress press;
    press.shift = KeyDown(VK_SHIFT);
    press.command = KeyDown(VK_CONTROL) || KeyDown(VK_MENU) || KeyDown(VK_LWIN) || KeyDown(VK_RWIN);
    press.control = KeyDown(VK_CONTROL) && !KeyDown(VK_MENU);
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
    // Ctrl+U, I, O, P and T as F6 to F10, as in Microsoft IME (for
    // keyboards without function keys); outside a composition they are
    // the application's.
    if (press.control && !press.shift) {
        constexpr std::pair<WPARAM, Key> kForms[] = {{'U', Key::F6}, {'I', Key::F7}, {'O', Key::F8}, {'P', Key::F9},
                                                     {'T', Key::F10}};
        for (const auto& [letter, key] : kForms) {
            if (virtualKey != letter) continue;
            press.key = key;
            press.command = false;
            return press;
        }
    }
    if (press.command) return press;
    if (kanaInput) {
        const UINT scan = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
        if (const wchar_t kana = KanaAt(scan, press.shift)) {
            press.key = Key::Character;
            press.character = kana;
            press.digit = 0;
            return press;
        }
    }
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
