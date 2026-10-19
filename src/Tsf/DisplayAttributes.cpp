#include "Tsf/DisplayAttributes.h"

#include "Tsf/TekitoGuids.h"

#include <oleauto.h>
#include <new>

namespace tekito::tsf {

HRESULT DisplayAttributeInfo::QueryInterface(REFIID riid, void** object) {
    if (!object) return E_INVALIDARG;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) {
        *object = static_cast<ITfDisplayAttributeInfo*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG DisplayAttributeInfo::AddRef() { return ++refCount_; }

ULONG DisplayAttributeInfo::Release() {
    const ULONG value = --refCount_;
    if (value == 0) delete this;
    return value;
}

HRESULT DisplayAttributeInfo::GetGUID(GUID* guid) {
    if (!guid) return E_INVALIDARG;
    *guid = guid_;
    return S_OK;
}

HRESULT DisplayAttributeInfo::GetDescription(BSTR* description) {
    if (!description) return E_INVALIDARG;
    *description = SysAllocString(description_);
    return *description ? S_OK : E_OUTOFMEMORY;
}

HRESULT DisplayAttributeInfo::GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) {
    if (!attribute) return E_INVALIDARG;
    *attribute = attribute_;
    return S_OK;
}

HRESULT DisplayAttributeInfo::SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) { return E_NOTIMPL; }

HRESULT DisplayAttributeInfo::Reset() { return S_OK; }

const GUID& JapaneseUnderlineGuid(JapaneseUnderline underline) noexcept {
    switch (underline) {
    case JapaneseUnderline::Converted: return GUID_TekitoDisplayAttributeConverted;
    case JapaneseUnderline::Focused: return GUID_TekitoDisplayAttributeFocused;
    case JapaneseUnderline::Settled: return GUID_TekitoDisplayAttributeSettled;
    case JapaneseUnderline::MixInput: return GUID_TekitoDisplayAttributeMixInput;
    case JapaneseUnderline::MixConverted: return GUID_TekitoDisplayAttributeMixConverted;
    case JapaneseUnderline::MixFocused: return GUID_TekitoDisplayAttributeMixFocused;
    case JapaneseUnderline::AutoInput: return GUID_TekitoDisplayAttributeAuto;
    case JapaneseUnderline::Input: break;
    }
    return GUID_TekitoDisplayAttributeInput;
}

ITfDisplayAttributeInfo* CreateJapaneseUnderline(JapaneseUnderline underline) {
    TF_DISPLAYATTRIBUTE attribute{};
    attribute.crText.type = TF_CT_NONE;
    attribute.crBk.type = TF_CT_NONE;
    attribute.crLine.type = TF_CT_SYSCOLOR;
    attribute.crLine.nIndex = COLOR_WINDOWTEXT;
    const wchar_t* description = L"TEKITO Japanese typed text";
    switch (underline) {
    case JapaneseUnderline::Input:
    case JapaneseUnderline::MixInput:
    case JapaneseUnderline::AutoInput:
        attribute.lsStyle = TF_LS_DOT;
        attribute.bAttr = TF_ATTR_INPUT;
        break;
    case JapaneseUnderline::Converted:
    case JapaneseUnderline::MixConverted:
        attribute.lsStyle = TF_LS_SOLID;
        attribute.bAttr = TF_ATTR_CONVERTED;
        description = L"TEKITO Japanese converted phrase";
        break;
    case JapaneseUnderline::Focused:
    case JapaneseUnderline::MixFocused:
        attribute.lsStyle = TF_LS_SOLID;
        attribute.fBoldLine = TRUE;
        attribute.bAttr = TF_ATTR_TARGET_CONVERTED;
        description = L"TEKITO Japanese phrase being converted";
        break;
    case JapaneseUnderline::Settled:
        // A marker in Mix's orange, the colour of its underline, with dark
        // text, readable on light and dark pages (white text would not be).
        attribute.lsStyle = TF_LS_NONE;
        attribute.crText.type = TF_CT_COLORREF;
        attribute.crText.cr = RGB(32, 32, 32);
        attribute.crBk.type = TF_CT_COLORREF;
        attribute.crBk.cr = RGB(245, 150, 25);
        attribute.bAttr = TF_ATTR_CONVERTED;
        description = L"TEKITO Mix sentence held until Enter";
        break;
    }
    // The mode's colour, as its icon has it: Mix orange, Auto blue.
    // Japanese keeps the text's colour.
    switch (underline) {
    case JapaneseUnderline::MixInput:
    case JapaneseUnderline::MixConverted:
    case JapaneseUnderline::MixFocused:
        attribute.crLine.type = TF_CT_COLORREF;
        attribute.crLine.cr = RGB(245, 150, 25);
        description = L"TEKITO Mix text";
        break;
    case JapaneseUnderline::AutoInput:
        attribute.crLine.type = TF_CT_COLORREF;
        attribute.crLine.cr = RGB(0, 103, 248);
        description = L"TEKITO Auto word";
        break;
    default:
        break;
    }
    return new (std::nothrow) DisplayAttributeInfo(JapaneseUnderlineGuid(underline), description, attribute);
}

}  // namespace tekito::tsf
