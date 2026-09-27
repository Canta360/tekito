#include "Core/Japanese/SortedTsv.h"

#include <algorithm>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace tekito::japanese {

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(std::max(length, 0)), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr,
                            nullptr);
    }
    return bytes;
}

std::wstring FromUtf8(std::string_view bytes) {
    if (bytes.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    }
    return text;
}

bool SortedTsv::Open(const std::filesystem::path& path) noexcept {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return false;
    return file_.Open(path);
}

std::size_t SortedTsv::LineStart(std::size_t at) const noexcept {
    // The first line that starts at or after `at`.
    if (at == 0) return 0;
    const auto* data = file_.Data();
    const std::size_t size = file_.Size();
    std::size_t i = at - 1;
    while (i < size && data[i] != '\n') ++i;
    return std::min(i + 1, size);
}

std::string_view SortedTsv::KeyAt(std::size_t lineStart) const noexcept {
    const auto* data = reinterpret_cast<const char*>(file_.Data());
    const std::size_t size = file_.Size();
    std::size_t end = lineStart;
    while (end < size && data[end] != '\t' && data[end] != '\n' && data[end] != '\r') ++end;
    return {data + lineStart, end - lineStart};
}

void SortedTsv::ForEachRow(std::string_view key,
                           const std::function<bool(const std::vector<std::string_view>& fields)>& row) const {
    if (!IsOpen() || key.empty()) return;
    const std::size_t size = file_.Size();
    std::size_t lo = 0, hi = size;
    while (lo < hi) {
        const std::size_t middle = lo + (hi - lo) / 2;
        const std::size_t start = LineStart(middle);
        if (start >= size || KeyAt(start) >= key) hi = middle; else lo = middle + 1;
    }
    const auto* data = reinterpret_cast<const char*>(file_.Data());
    std::vector<std::string_view> fields;
    for (std::size_t start = LineStart(lo); start < size;) {
        if (KeyAt(start) != key) return;
        std::size_t end = start;
        while (end < size && data[end] != '\n') ++end;
        std::string_view line(data + start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fields.clear();
        for (std::size_t from = 0;;) {
            const std::size_t tab = line.find('\t', from);
            fields.push_back(line.substr(from, tab == std::string_view::npos ? std::string_view::npos : tab - from));
            if (tab == std::string_view::npos) break;
            from = tab + 1;
        }
        if (!row(fields)) return;
        start = end + 1;
    }
}

}  // namespace tekito::japanese
