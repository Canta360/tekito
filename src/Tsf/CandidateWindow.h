#pragma once

#include "Core/Candidate.h"
#include "Core/InputMode.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
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
// What the highlighted candidate means, shown in a pane beside the list.
struct CandidateDetail {
    std::wstring headword;
    std::vector<std::wstring> senses;
};

class CandidateWindow final {
public:
    CandidateWindow();
    ~CandidateWindow();
    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;

    bool Initialize(HINSTANCE instance, std::function<void(std::size_t)> onSelection);
    // selectedIndex kNoSelection highlights nothing (predictions before the
    // user steps into them).
    static constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);
    void Show(const RECT& caretRect,
              const std::vector<Candidate>& candidates,
              std::size_t selectedIndex,
              std::size_t pageStart,
              std::size_t visibleCount,
              const CandidateDetail& detail = {});
    void Hide() noexcept;
    // The mode's icon alone, in the list's style, under the caret for a
    // moment after the user switches modes; it fades by itself. The next
    // Show replaces it.
    void ShowModeBadge(const RECT& caretRect, InputMode mode);
    bool IsShown() const noexcept { return shown_; }
    // 0 = glass, 1 = simple (see UserSettings::candidateWindowStyle). Takes
    // effect from the next Show.
    void SetStyle(int style) noexcept;
    // Japanese tags (そのまま, スラング, 絵文字) instead of English ones.
    // Takes effect from the next Show.
    void SetJapanese(bool japanese) noexcept { japanese_ = japanese; }
    // Told about every list before it is drawn, and when it goes, for an
    // application that may show the list itself (a full-screen game: TSF UI
    // elements). `shown` returns false when the application shows it: this
    // window then stays hidden.
    using ListShown = std::function<bool(const std::vector<Candidate>& candidates, std::size_t selectedIndex,
                                         std::size_t pageStart, std::size_t visibleCount)>;
    void SetListObserver(ListShown shown, std::function<void()> hidden) {
        listShown_ = std::move(shown);
        listHidden_ = std::move(hidden);
    }

    struct Channel;

private:
    static LRESULT CALLBACK NotifyWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void Shutdown() noexcept;
    // Hide without telling the list observer.
    void HideWindow() noexcept;

    HINSTANCE instance_{nullptr};
    HWND notifyHwnd_{nullptr};
    HANDLE uiThread_{nullptr};
    std::shared_ptr<Channel> channel_;
    std::function<void(std::size_t)> onSelection_;
    ListShown listShown_;
    std::function<void()> listHidden_;

    // Host-thread copy of what is on screen, used to resolve click selections.
    std::vector<Candidate> candidates_;
    std::size_t selectedIndex_{0};
    std::size_t pageStart_{0};
    std::size_t visibleCount_{0};
    std::uint64_t generation_{0};
    bool shown_{false};
    bool japanese_{false};
    int style_{0};
};

}  // namespace tekito::tsf
