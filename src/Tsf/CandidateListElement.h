#pragma once

#include "Core/Candidate.h"

#include <msctf.h>
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <functional>
#include <vector>

namespace tekito::tsf {

// The candidate list as a TSF UI element: an application that draws the
// input method's UI itself (a full-screen game) reads the candidates from
// it, and tells whether TEKITO's own window is shown.
class CandidateListElement final : public ITfCandidateListUIElementBehavior {
public:
    // `choose` commits the candidate at an index (the application picked
    // it); `threadManager` gives the document the list is for.
    CandidateListElement(ITfThreadMgr* threadManager, std::function<void(std::size_t)> choose);

    // ITfCandidateListUIElementBehavior (with ITfCandidateListUIElement and
    // ITfUIElement).
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override;
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override;
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* shown) override;
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* flags) override;
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** documentManager) override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override;
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* index) override;
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR* text) override;
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* index, UINT size, UINT* pageCount) override;
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* index, UINT pageCount) override;
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* page) override;
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override;
    HRESULT STDMETHODCALLTYPE Finalize() override;
    HRESULT STDMETHODCALLTYPE Abort() override;

    // CandidateWindow::ListShown: begins or updates the element; whether
    // TEKITO's window should draw the list.
    bool OnListShown(const std::vector<Candidate>& candidates, std::size_t selectedIndex, std::size_t pageStart,
                     std::size_t visibleCount);
    // The list went: the element ends.
    void OnListHidden();
    // Drops the thread manager (the text service is deactivating).
    void Detach();

private:
    ~CandidateListElement();
    [[nodiscard]] std::size_t PageSize() const noexcept;

    std::atomic<ULONG> refCount_{1};
    ITfThreadMgr* threadManager_{nullptr};
    std::function<void(std::size_t)> choose_;
    std::vector<Candidate> candidates_;
    std::size_t selected_{0};
    std::size_t pageStart_{0};
    std::size_t visibleCount_{0};
    DWORD updated_{0};
    DWORD elementId_{TF_INVALID_UIELEMENTID};
    bool begun_{false};
    // The application lets TEKITO draw the list (BeginUIElement, Show).
    bool shown_{true};
};

}  // namespace tekito::tsf
