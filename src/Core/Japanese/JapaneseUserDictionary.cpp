#include "Core/Japanese/JapaneseUserDictionary.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace tekito::japanese {
namespace {

constexpr std::array<std::pair<UserWordKind, std::wstring_view>, 10> kKindNames{{
    {UserWordKind::Noun, L"noun"},
    {UserWordKind::ProperNoun, L"proper-noun"},
    {UserWordKind::Person, L"person"},
    {UserWordKind::Surname, L"surname"},
    {UserWordKind::GivenName, L"given-name"},
    {UserWordKind::Place, L"place"},
    {UserWordKind::Organization, L"organization"},
    {UserWordKind::SuruNoun, L"suru-noun"},
    {UserWordKind::Symbol, L"symbol"},
    {UserWordKind::Interjection, L"interjection"},
}};

constexpr std::array<std::pair<UserWordAction, std::wstring_view>, 3> kActionNames{{
    {UserWordAction::First, L"first"},
    {UserWordAction::Suggest, L"suggest"},
    {UserWordAction::Suppress, L"suppress"},
}};

template <typename Number>
std::optional<Number> ParseNumber(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const auto tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) break;
        start = tab + 1;
    }
    return fields;
}

}  // namespace

std::wstring_view KindName(UserWordKind kind) noexcept {
    for (const auto& [value, name] : kKindNames) {
        if (value == kind) return name;
    }
    return L"noun";
}

std::optional<UserWordKind> KindFromName(std::wstring_view name) noexcept {
    for (const auto& [value, known] : kKindNames) {
        if (known == name) return value;
    }
    return std::nullopt;
}

std::wstring_view ActionName(UserWordAction action) noexcept {
    for (const auto& [value, name] : kActionNames) {
        if (value == action) return name;
    }
    return L"first";
}

std::optional<UserWordAction> ActionFromName(std::wstring_view name) noexcept {
    for (const auto& [value, known] : kActionNames) {
        if (known == name) return value;
    }
    return std::nullopt;
}

UserPartsOfSpeech UserPartsOfSpeech::Load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return Parse(text.str());
}

UserPartsOfSpeech UserPartsOfSpeech::Parse(std::string_view text) {
    UserPartsOfSpeech parts;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const auto fields = SplitTabs(line);
        if (fields.size() < 4) continue;
        // The names are ASCII.
        const std::wstring name(fields[0].begin(), fields[0].end());
        const auto kind = KindFromName(name);
        const auto left = ParseNumber<std::uint16_t>(fields[1]);
        const auto right = ParseNumber<std::uint16_t>(fields[2]);
        const auto cost = ParseNumber<std::int32_t>(fields[3]);
        if (!left || !right || !cost) continue;
        if (kind) parts.entries_[*kind] = {*left, *right, *cost};
        if (name == L"number") parts.number_ = Entry{*left, *right, *cost};
    }
    return parts;
}

std::optional<UserPartsOfSpeech::Entry> UserPartsOfSpeech::For(UserWordKind kind) const noexcept {
    const auto found = entries_.find(kind);
    if (found == entries_.end()) return std::nullopt;
    return found->second;
}

void JapaneseUserDictionary::Set(const std::vector<UserWord>& words, const UserPartsOfSpeech& parts) {
    byReading_.clear();
    suggested_.clear();
    suppressed_.clear();
    longest_ = 0;
    size_ = 0;
    for (const auto& word : words) {
        if (word.reading.empty() || word.surface.empty()) continue;
        if (word.action == UserWordAction::Suppress) {
            auto& surfaces = suppressed_[word.reading];
            if (std::find(surfaces.begin(), surfaces.end(), word.surface) == surfaces.end()) {
                surfaces.push_back(word.surface);
                ++size_;
            }
            continue;
        }
        const auto part = parts.For(word.kind);
        if (!part) continue;
        if (word.action == UserWordAction::Suggest) {
            auto& entries = suggested_[word.reading];
            const bool duplicate = std::any_of(entries.begin(), entries.end(),
                                               [&](const Entry& e) { return e.surface == word.surface; });
            if (!duplicate) {
                entries.push_back({word.reading, word.surface, part->left, part->right, part->cost});
                ++size_;
            }
            continue;
        }
        auto& entries = byReading_[word.reading];
        const bool duplicate = std::any_of(entries.begin(), entries.end(),
                                           [&](const Entry& e) { return e.surface == word.surface; });
        if (duplicate) continue;
        entries.push_back({word.reading, word.surface, part->left, part->right, part->cost});
        longest_ = std::max(longest_, word.reading.size());
        ++size_;
    }
}

const std::vector<JapaneseUserDictionary::Entry>* JapaneseUserDictionary::Exact(std::wstring_view reading) const {
    const auto found = byReading_.find(reading);
    return found == byReading_.end() ? nullptr : &found->second;
}

const std::vector<JapaneseUserDictionary::Entry>* JapaneseUserDictionary::Suggested(std::wstring_view reading) const {
    const auto found = suggested_.find(reading);
    return found == suggested_.end() ? nullptr : &found->second;
}

bool JapaneseUserDictionary::Suppresses(std::wstring_view reading, std::wstring_view surface) const {
    const auto found = suppressed_.find(reading);
    if (found == suppressed_.end()) return false;
    return std::find(found->second.begin(), found->second.end(), surface) != found->second.end();
}

std::vector<const JapaneseUserDictionary::Entry*> JapaneseUserDictionary::StartingWith(std::wstring_view prefix,
                                                                                         bool suggested) const {
    std::vector<const Entry*> found;
    if (prefix.empty()) return found;
    const auto& words = suggested ? suggested_ : byReading_;
    for (auto it = words.lower_bound(prefix); it != words.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) break;
        if (it->first.size() <= prefix.size()) continue;
        for (const auto& entry : it->second) found.push_back(&entry);
    }
    return found;
}

}  // namespace tekito::japanese
