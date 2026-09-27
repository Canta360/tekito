#pragma once

#include <msctf.h>
#include <atomic>

namespace tekito::tsf {

// How Japanese composition text is underlined: typed text dotted,
// converted phrases thin, the phrase being converted thick. (The English
// word's underline is the text service itself.)
class DisplayAttributeInfo final : public ITfDisplayAttributeInfo {
public:
    DisplayAttributeInfo(const GUID& guid, const wchar_t* description,
                         const TF_DISPLAYATTRIBUTE& attribute) noexcept
        : guid_(guid), description_(description), attribute_(attribute) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override;
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override;
    HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) override;
    HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE* attribute) override;
    HRESULT STDMETHODCALLTYPE Reset() override;

private:
    ~DisplayAttributeInfo() = default;

    std::atomic<ULONG> refCount_{1};
    GUID guid_;
    const wchar_t* description_;
    TF_DISPLAYATTRIBUTE attribute_;
};

// The three Japanese underlines, in the order input, converted, focused;
// new references the caller releases.
enum class JapaneseUnderline { Input, Converted, Focused };
inline constexpr int kJapaneseUnderlineCount = 3;
[[nodiscard]] const GUID& JapaneseUnderlineGuid(JapaneseUnderline underline) noexcept;
[[nodiscard]] ITfDisplayAttributeInfo* CreateJapaneseUnderline(JapaneseUnderline underline);

}  // namespace tekito::tsf
