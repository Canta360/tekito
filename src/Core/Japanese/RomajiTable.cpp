#include "Core/Japanese/RomajiTable.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
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
    table.packRules_ = table.rules_;
    table.Index();
    return table;
}

void RomajiTable::Index() {
    keysForKana_.clear();
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
    for (const auto& [keys, rule] : rules_) {
        if (!rule.pending.empty()) continue;
        auto [it, inserted] = keysForKana_.try_emplace(rule.output, keys);
        if (!inserted && better(keys, it->second)) it->second = keys;
    }
}

void RomajiTable::ApplyChanges(const std::vector<Change>& changes) {
    rules_ = packRules_;
    for (const auto& change : changes) {
        if (change.input.empty()) continue;
        if (change.output.empty()) {
            rules_.erase(change.input);
        } else {
            rules_.insert_or_assign(change.input, RomajiRule{change.output, change.pending});
        }
    }
    Index();
}

const RomajiRule* RomajiTable::PackRule(std::wstring_view input) const {
    const auto it = packRules_.find(input);
    return it == packRules_.end() ? nullptr : &it->second;
}

std::vector<RomajiTable::Change> RomajiTable::ChangesFor(const std::vector<Change>& table) const {
    std::vector<Change> changes;
    std::map<std::wstring, const Change*, std::less<>> rows;
    for (const auto& row : table) rows.insert_or_assign(row.input, &row);
    for (const auto& [input, row] : rows) {
        const auto* pack = PackRule(input);
        if (!pack || pack->output != row->output || pack->pending != row->pending) changes.push_back(*row);
    }
    // A whole table (not a few rows to add): what it leaves out is not used.
    constexpr std::size_t kWholeTable = 100;
    if (rows.size() >= kWholeTable) {
        for (const auto& [input, rule] : packRules_) {
            // Marks and letters the table never lists stay as the pack has them.
            const bool letters = std::any_of(input.begin(), input.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
            if (letters && !rows.contains(input)) changes.push_back({input, {}, {}});
        }
    }
    return changes;
}

std::vector<RomajiTable::Change> RomajiTable::ReadChanges(const std::filesystem::path& file) {
    std::vector<Change> changes;
    std::ifstream in(file, std::ios::binary);
    if (!in) return changes;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::wstring text = FromUtf8(bytes);
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        auto line = std::wstring_view(text).substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const auto fields = SplitTabs(line);
        if (fields.size() != 3 || fields[0].empty()) continue;
        changes.push_back({std::wstring(fields[0]), std::wstring(fields[1]), std::wstring(fields[2])});
    }
    return changes;
}

bool RomajiTable::WriteChanges(const std::filesystem::path& file, const std::vector<Change>& changes) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::wstring text;
    for (const auto& change : changes) text += change.input + L"\t" + change.output + L"\t" + change.pending + L"\n";
#if defined(_WIN32)
    std::string bytes;
    if (!text.empty()) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                               nullptr, nullptr);
        bytes.resize(static_cast<std::size_t>(length));
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr,
                            nullptr);
    }
#else
    std::string bytes(text.begin(), text.end());
#endif
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::vector<RomajiTable::Change> RomajiTable::ParseTable(const std::filesystem::path& file) {
    std::vector<Change> rows;
    std::ifstream in(file, std::ios::binary);
    if (!in) return rows;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    } else {
        std::string_view utf8 = bytes;
        if (utf8.starts_with("\xEF\xBB\xBF")) utf8.remove_prefix(3);
        text = FromUtf8(utf8);
#if defined(_WIN32)
        if (text.empty() && !utf8.empty()) {
            // Not UTF-8: Shift_JIS, as older Microsoft IME files are.
            const int length = MultiByteToWideChar(932, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (length > 0) {
                text.resize(static_cast<std::size_t>(length));
                MultiByteToWideChar(932, 0, utf8.data(), static_cast<int>(utf8.size()), text.data(), length);
            }
        }
#endif
    }
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        auto line = std::wstring_view(text).substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const auto fields = SplitTabs(line);
        if (fields.size() < 2 || fields[0].empty() || fields[1].empty()) continue;
        // Keys are what a keyboard types (a header row is not).
        if (std::any_of(fields[0].begin(), fields[0].end(), [](wchar_t c) { return c < L'!' || c > L'~'; })) continue;
        rows.push_back({std::wstring(fields[0]), std::wstring(fields[1]),
                        fields.size() > 2 ? std::wstring(fields[2]) : std::wstring()});
    }
    return rows;
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
