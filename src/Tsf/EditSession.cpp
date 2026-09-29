#include "Tsf/EditSession.h"
#include "Tsf/TextService.h"

namespace tekito::tsf {

KeyEditSession::KeyEditSession(TextService* service, ITfContext* context, KeyInput input)
    : service_(service), context_(context), input_(input) {
    if (service_) service_->AddRef();
    if (context_) context_->AddRef();
}

HRESULT KeyEditSession::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
        *object = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG KeyEditSession::AddRef() { return ++refCount_; }

ULONG KeyEditSession::Release() {
    const ULONG value = --refCount_;
    if (value == 0) {
        if (context_) context_->Release();
        if (service_) service_->Release();
        delete this;
    }
    return value;
}

HRESULT ReadEditSession::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
        *object = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ReadEditSession::Release() {
    const ULONG value = --refCount_;
    if (value == 0) delete this;
    return value;
}

HRESULT ReadEditSession::DoEditSession(TfEditCookie editCookie) {
    if (read_) read_(editCookie);
    return S_OK;
}

HRESULT KeyEditSession::DoEditSession(TfEditCookie editCookie) {
    return service_ ? service_->HandleKeyInEditSession(context_, editCookie, input_) : E_UNEXPECTED;
}

}  // namespace tekito::tsf
