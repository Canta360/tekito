#pragma once

#include "Core/Candidate.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tekito::tsf {

// Vertical candidate list popup.
//
// The TSF calls Show/Hide on the host application's UI thread. Those calls
// only snapshot the visible rows and post them; layout, text measurement and
// drawing run on a dedicated UI thread owned by this class. That keeps the
// host thread free of rendering cost and keeps the Windows.UI.Composition
// machinery the glass surface needs (a per-thread DispatcherQueue) off
// threads the host application owns. Click selections come back to the host
// thread through a message-only window, so onSelection always runs on the
// thread that called Initialize.
class CandidateWindow final {
public:
    CandidateWindow();
    ~CandidateWindow();
    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;

    bool Initialize(HINSTANCE instance, std::function<void(std::size_t)> onSelection);
    void Show(const RECT& caretRect,
              const std::vector<Candidate>& candidates,
              std::size_t selectedIndex,
              std::size_t pageStart,
              std::size_t visibleCount);
    void Hide() noexcept;
    bool IsShown() const noexcept { return shown_; }
    // 0 = glass, 1 = simple (see UserSettings::candidateWindowStyle). Takes
    // effect from the next Show.
    void SetStyle(int style) noexcept;

    struct Channel;

private:
    static LRESULT CALLBACK NotifyWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void Shutdown() noexcept;

    HINSTANCE instance_{nullptr};
    HWND notifyHwnd_{nullptr};
    HANDLE uiThread_{nullptr};
    std::shared_ptr<Channel> channel_;
    std::function<void(std::size_t)> onSelection_;

    // Host-thread copy of what is on screen, used to resolve click selections.
    std::vector<Candidate> candidates_;
    std::size_t selectedIndex_{0};
    std::size_t pageStart_{0};
    std::size_t visibleCount_{0};
    std::uint64_t generation_{0};
    bool shown_{false};
    int style_{0};
};

}  // namespace tekito::tsf
