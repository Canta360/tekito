#pragma once

#include "Core/InputMode.h"

#include <windows.h>

namespace tekito::tsf {

// The mode's icon on a small tile by the caret for a moment after the user
// switches modes, as Microsoft IME shows it. A layered window on the host
// thread: it never takes focus, and clicks go through it.
class ModeIndicator final {
public:
    ModeIndicator() = default;
    ~ModeIndicator();
    ModeIndicator(const ModeIndicator&) = delete;
    ModeIndicator& operator=(const ModeIndicator&) = delete;

    // `caret` is the screen rectangle of the caret or selection.
    void Show(HINSTANCE instance, const RECT& caret, InputMode mode);
    void Hide() noexcept;
    // Destroys the window; call before the module can unload.
    void Shutdown() noexcept;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureWindow(HINSTANCE instance);
    bool Render(HINSTANCE instance, InputMode mode, UINT dpi);
    void Present(BYTE opacity) noexcept;
    void ReleaseFrame() noexcept;

    HWND hwnd_{nullptr};
    // The drawn tile, kept for the fade.
    HDC frameDc_{nullptr};
    HBITMAP frame_{nullptr};
    HGDIOBJ previousBitmap_{nullptr};
    POINT position_{};
    SIZE size_{};
    int fadeStep_{0};
};

}  // namespace tekito::tsf
