#include "Tsf/ClassFactory.h"

#include "Tsf/Globals.h"
#include "Tsf/TextService.h"

#include <new>

namespace tekito::tsf {

HRESULT ClassFactory::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ClassFactory::AddRef() { return ++refCount_; }

ULONG ClassFactory::Release() {
    const ULONG value = --refCount_;
    if (value == 0) delete this;
    return value;
}

HRESULT ClassFactory::CreateInstance(IUnknown* outer, REFIID riid, void** object) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!object) return E_INVALIDARG;

    auto* service = new (std::nothrow) TextService();
    if (!service) return E_OUTOFMEMORY;
    const HRESULT hr = service->QueryInterface(riid, object);
    service->Release();
    return hr;
}

HRESULT ClassFactory::LockServer(BOOL lock) {
    if (lock) ++g_serverLockCount;
    else --g_serverLockCount;
    return S_OK;
}

}  // namespace tekito::tsf
