#include "Tsf/CandidateListElement.h"

#include "Tsf/ComPtr.h"
#include "Tsf/Diagnostics.h"

#include <algorithm>

namespace tekito::tsf {
namespace {

// {6C2E1F4B-0B9A-4C55-9E7D-3A1F2B8C4D60}: the candidate list element.
constexpr GUID kCandidateListElementGuid = {0x6c2e1f4b, 0x0b9a, 0x4c55, {0x9e, 0x7d, 0x3a, 0x1f, 0x2b, 0x8c, 0x4d, 0x60}};
constexpr DWORD kAllUpdated = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING |
                              TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;

}  // namespace

CandidateListElement::CandidateListElement(ITfThreadMgr* threadManager, std::function<void(std::size_t)> choose)
    : threadManager_(threadManager), choose_(std::move(choose)) {
    if (threadManager_) threadManager_->AddRef();
}

CandidateListElement::~CandidateListElement() {
    if (threadManager_) threadManager_->Release();
}

HRESULT CandidateListElement::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfUIElement) ||
        IsEqualIID(riid, IID_ITfCandidateListUIElement) || IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior)) {
        *object = static_cast<ITfCandidateListUIElementBehavior*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG CandidateListElement::Release() {
    const ULONG count = --refCount_;
    if (count == 0) delete this;
    return count;
}

HRESULT CandidateListElement::GetDescription(BSTR* description) {
    if (!description) return E_INVALIDARG;
    *description = SysAllocString(L"TEKITO candidates");
    return *description ? S_OK : E_OUTOFMEMORY;
}

HRESULT CandidateListElement::GetGUID(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    *guid = kCandidateListElementGuid;
    return S_OK;
}

HRESULT CandidateListElement::Show(BOOL show) {
    shown_ = show != FALSE;
    return S_OK;
}

HRESULT CandidateListElement::IsShown(BOOL* shown) {
    if (!shown) return E_INVALIDARG;
    *shown = begun_ && shown_ ? TRUE : FALSE;
    return S_OK;
}

HRESULT CandidateListElement::GetUpdatedFlags(DWORD* flags) {
    if (!flags) return E_INVALIDARG;
    *flags = updated_;
    updated_ = 0;
    return S_OK;
}

HRESULT CandidateListElement::GetDocumentMgr(ITfDocumentMgr** documentManager) {
    if (!documentManager) return E_INVALIDARG;
    *documentManager = nullptr;
    if (!threadManager_) return E_FAIL;
    return threadManager_->GetFocus(documentManager);
}

HRESULT CandidateListElement::GetCount(UINT* count) {
    if (!count) return E_INVALIDARG;
    *count = static_cast<UINT>(candidates_.size());
    return S_OK;
}

HRESULT CandidateListElement::GetSelection(UINT* index) {
    if (!index) return E_INVALIDARG;
    // Predictions before one is stepped into: the first.
    *index = selected_ < candidates_.size() ? static_cast<UINT>(selected_) : 0;
    return S_OK;
}

HRESULT CandidateListElement::GetString(UINT index, BSTR* text) {
    if (!text) return E_INVALIDARG;
    *text = nullptr;
    if (index >= candidates_.size()) return E_INVALIDARG;
    *text = SysAllocString(candidates_[index].text.c_str());
    return *text ? S_OK : E_OUTOFMEMORY;
}

std::size_t CandidateListElement::PageSize() const noexcept {
    // The pages are as long as the one shown (the last may be shorter).
    return std::max<std::size_t>(visibleCount_, 1);
}

HRESULT CandidateListElement::GetPageIndex(UINT* index, UINT size, UINT* pageCount) {
    if (!pageCount) return E_INVALIDARG;
    const std::size_t page = PageSize();
    // The page shown starts at pageStart_; the others at whole pages from it.
    std::vector<UINT> starts;
    for (std::size_t start = pageStart_ % page; start < candidates_.size(); start += page) {
        starts.push_back(static_cast<UINT>(start));
    }
    if (pageStart_ % page != 0) starts.insert(starts.begin(), 0);
    *pageCount = static_cast<UINT>(starts.size());
    if (!index) return S_OK;
    if (size < starts.size()) return E_NOT_SUFFICIENT_BUFFER;
    std::copy(starts.begin(), starts.end(), index);
    return S_OK;
}

HRESULT CandidateListElement::SetPageIndex(UINT*, UINT) {
    return E_NOTIMPL;
}

HRESULT CandidateListElement::GetCurrentPage(UINT* page) {
    if (!page) return E_INVALIDARG;
    UINT count = 0;
    (void)GetPageIndex(nullptr, 0, &count);
    const std::size_t size = PageSize();
    const std::size_t before = pageStart_ % size != 0 ? 1 : 0;
    *page = static_cast<UINT>(std::min<std::size_t>(pageStart_ / size + before, count ? count - 1 : 0));
    return S_OK;
}

HRESULT CandidateListElement::SetSelection(UINT index) {
    if (index >= candidates_.size()) return E_INVALIDARG;
    selected_ = index;
    return S_OK;
}

HRESULT CandidateListElement::Finalize() {
    if (selected_ >= candidates_.size() || !choose_) return E_FAIL;
    choose_(selected_);
    return S_OK;
}

HRESULT CandidateListElement::Abort() {
    return E_NOTIMPL;
}

bool CandidateListElement::OnListShown(const std::vector<Candidate>& candidates, std::size_t selectedIndex,
                                       std::size_t pageStart, std::size_t visibleCount) {
    DWORD changed = 0;
    if (candidates.size() != candidates_.size()) changed |= TF_CLUIE_COUNT;
    if (candidates != candidates_) changed |= TF_CLUIE_STRING;
    if (selectedIndex != selected_) changed |= TF_CLUIE_SELECTION;
    if (pageStart != pageStart_ || visibleCount != visibleCount_) changed |= TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    candidates_ = candidates;
    selected_ = selectedIndex;
    pageStart_ = pageStart;
    visibleCount_ = visibleCount;
    if (!threadManager_) return true;
    ComPtr<ITfUIElementMgr> elements;
    if (FAILED(threadManager_->QueryInterface(IID_PPV_ARGS(elements.Put())))) return true;
    if (!begun_) {
        BOOL show = TRUE;
        DWORD id = TF_INVALID_UIELEMENTID;
        updated_ = kAllUpdated;
        if (FAILED(elements->BeginUIElement(this, &show, &id))) return true;
        begun_ = true;
        elementId_ = id;
        shown_ = show != FALSE;
        if (!shown_) Trace(L"Candidate list drawn by the application");
        return shown_;
    }
    updated_ |= changed;
    if (changed) (void)elements->UpdateUIElement(elementId_);
    return shown_;
}

void CandidateListElement::OnListHidden() {
    if (!begun_) return;
    begun_ = false;
    shown_ = true;
    ComPtr<ITfUIElementMgr> elements;
    if (threadManager_ && SUCCEEDED(threadManager_->QueryInterface(IID_PPV_ARGS(elements.Put())))) {
        (void)elements->EndUIElement(elementId_);
    }
    elementId_ = TF_INVALID_UIELEMENTID;
}

void CandidateListElement::Detach() {
    OnListHidden();
    if (threadManager_) threadManager_->Release();
    threadManager_ = nullptr;
    choose_ = nullptr;
}

}  // namespace tekito::tsf
