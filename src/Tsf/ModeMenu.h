#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace tekito::tsf {

// The taskbar mode button's menu, drawn as Windows 11 draws its flyouts:
// rounded, on acrylic where Windows shows transparency, in the app theme,
// the current mode marked in the accent color. Without a window to borrow
// (its own window owns it), so it works in apps whose text input runs
// apart from their window. High contrast gets the standard menu instead
// (ModeLangBarItem).
struct ModeMenuItem {
    enum class Kind { Mode, Toggle, Command, Separator };
    Kind kind{Kind::Command};
    UINT id{0};
    std::wstring label;
    // Mode: the mode's icon resource; Toggle and Command: a Segoe Fluent
    // Icons glyph.
    int icon{0};
    wchar_t glyph{0};
    // Mode: the current one; Toggle: on.
    bool checked{false};
};

class ModeMenu final {
public:
    // Shows the menu at the pointer and returns when it closes. `choose` is
    // called with a chosen item's id: a mode or a command closes the menu
    // first; a toggle stays open, and `checked` says what it shows now.
    // Clicking the button again while it is open (Open from inside) closes it.
    static void Show(HINSTANCE instance, std::vector<ModeMenuItem> items,
                     const std::function<void(UINT id)>& choose, const std::function<bool(UINT id)>& checked);
    // False in high contrast, where the standard menu is shown instead.
    [[nodiscard]] static bool Available();
    [[nodiscard]] static bool IsOpen();
    static void Close();
};

}  // namespace tekito::tsf
