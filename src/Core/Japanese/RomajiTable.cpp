#include "Core/Japanese/RomajiTable.h"

#include <fstream>
#include <iterator>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace tekito::japanese {
namespace {

std::wstring FromUtf8(std::string_view bytes) {
#if defined(_WIN32)
    if (bytes.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                        out.data(), length);
    return out;
#else
    return std::wstring(bytes.begin(), bytes.end());
#endif
}

std::vector<std::wstring_view> SplitTabs(std::wstring_view line) {
    std::vector<std::wstring_view> fields;
    std::size_t start = 0;
    while (true) {
        const auto tab = line.find(L'\t', start);
        fields.push_back(line.substr(start, tab == std::wstring_view::npos ? tab : tab - start));
        if (tab == std::wstring_view::npos) break;
        start = tab + 1;
    }
    return fields;
}

}  // namespace

RomajiTable RomajiTable::FromTsv(std::wstring_view text) {
    RomajiTable table;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L'\n', start);
        if (end == std::wstring_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        // No comments: "#" is itself a key the table maps.
        if (line.empty()) continue;
        const auto fields = SplitTabs(line);
        if (fields.size() != 3 || fields[0].empty() || fields[1].empty()) continue;
        table.rules_.insert_or_assign(std::wstring(fields[0]),
                                      RomajiRule{std::wstring(fields[1]), std::wstring(fields[2])});
    }
    // For each kana the shortest spelling; among equally short ones, the
    // usual one rather than an alternative ("ka", not "ca"; "a", not "xa").
    const auto alternative = [](std::wstring_view keys) {
        return std::wstring_view(L"clqx").find(keys.front()) != std::wstring_view::npos;
    };
    const auto better = [&](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return a.size() < b.size();
        if (alternative(a) != alternative(b)) return !alternative(a);
        return a < b;
    };
    for (const auto& [keys, rule] : table.rules_) {
        if (!rule.pending.empty()) continue;
        auto [it, inserted] = table.keysForKana_.try_emplace(rule.output, keys);
        if (!inserted && better(keys, it->second)) it->second = keys;
    }
    return table;
}

std::unique_ptr<RomajiTable> RomajiTable::Load(const std::filesystem::path& packDirectory) {
    std::ifstream file(packDirectory / L"romaji.tsv", std::ios::binary);
    if (!file) return nullptr;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto table = std::make_unique<RomajiTable>(FromTsv(FromUtf8(bytes)));
    if (table->Empty()) return nullptr;
    return table;
}

const RomajiRule* RomajiTable::Find(std::wstring_view keys) const {
    const auto it = rules_.find(keys);
    return it == rules_.end() ? nullptr : &it->second;
}

bool RomajiTable::HasLongerInput(std::wstring_view keys) const {
    auto it = rules_.lower_bound(keys);
    if (it != rules_.end() && it->first == keys) ++it;
    return it != rules_.end() && it->first.starts_with(keys);
}

std::wstring RomajiTable::KeysFor(std::wstring_view kana) const {
    const auto it = keysForKana_.find(kana);
    return it == keysForKana_.end() ? std::wstring{} : it->second;
}

}  // namespace tekito::japanese
