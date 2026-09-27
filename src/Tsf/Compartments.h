#pragma once

#include "Tsf/ComPtr.h"

#include <msctf.h>

namespace tekito::tsf {

inline ComPtr<ITfCompartment> ThreadCompartment(ITfThreadMgr* threadManager, REFGUID guid) {
    ComPtr<ITfCompartmentMgr> manager;
    ComPtr<ITfCompartment> compartment;
    if (threadManager && SUCCEEDED(threadManager->QueryInterface(IID_PPV_ARGS(manager.Put())))) {
        (void)manager->GetCompartment(guid, compartment.Put());
    }
    return compartment;
}

inline bool ReadCompartment(ITfCompartment* compartment, LONG& value) {
    if (!compartment) return false;
    VARIANT variant;
    VariantInit(&variant);
    const bool read = SUCCEEDED(compartment->GetValue(&variant)) && variant.vt == VT_I4;
    if (read) value = variant.lVal;
    VariantClear(&variant);
    return read;
}

// Writes the value unless it is already there, so listeners hear only real
// changes.
inline void WriteCompartment(ITfCompartment* compartment, TfClientId clientId, LONG value) {
    LONG current = 0;
    if (!compartment || (ReadCompartment(compartment, current) && current == value)) return;
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_I4;
    variant.lVal = value;
    (void)compartment->SetValue(clientId, &variant);
}

}  // namespace tekito::tsf
