#include "Dictionary/ExternalDictionaryProvider.h"

#include "Core/ExternalLexiconProvider.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string_view>

namespace tekito::dictionary {
namespace {

constexpr std::string_view kIndexHeader = "TEKITO_DICTIONARY_INDEX_V1";
constexpr std::string_view kPronunciationIndexHeader = "TEKITO_PRONUNCIATION_INDEX_V1";

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

std::string ToAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch < 0 || ch > 0x7f) return {};
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::wstring ToWide(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char ch : value) result.push_back(static_cast<wchar_t>(ch));
    return result;
}

std::filesystem::path Absolute(std::filesystem::path path,
                               const std::filesystem::path& base = {}) {
    if (path.empty()) return {};
    if (path.is_relative() && !base.empty()) path = base / path;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return error ? std::filesystem::path{} : absolute;
}

bool Parse(std::string_view line, DictionaryEntry& entry, std::string& key) {
    if (line.empty() || line.front() == '#') return false;
    std::string_view fields[6];
    for (auto& field : fields) {
        const auto separator = line.find('\t');
        field = separator == std::string_view::npos ? line : line.substr(0, separator);
        field = Trim(field);
        line = separator == std::string_view::npos ? std::string_view{} : line.substr(separator + 1);
    }
    if (fields[0].empty()) return false;
    key.assign(fields[0]);
    entry = {ToWide(fields[0]), ToWide(fields[1]), ToWide(fields[2]), ToWide(fields[3]),
             ToWide(fields[4]), ToWide(fields[5])};
    return true;
}

}  // namespace

ExternalDictionaryProvider::ExternalDictionaryProvider(std::filesystem::path path)
    : path_(Absolute(std::move(path), DataPackRoot())), indexPath_(IndexPath(path_)) {
    if (!path_.empty() && !indexPath_.empty()) {
        std::ifstream input(path_, std::ios::binary);
        loaded_ = static_cast<bool>(input) && LoadIndex();
    }
}

std::filesystem::path ExternalDictionaryProvider::DataPackRoot() {
    return ExternalLexiconProvider::DataPackRoot();
}

std::filesystem::path ExternalDictionaryProvider::DefaultPath() {
    const auto root = DataPackRoot();
    return root.empty() ? std::filesystem::path{} : root / L"dictionary-display" / L"entries.tsv";
}

std::filesystem::path ExternalDictionaryProvider::IndexPath(
    const std::filesystem::path& dataPath) {
    if (dataPath.empty()) return {};
    auto result = dataPath;
    result += L".idx";
    return result;
}

bool ExternalDictionaryProvider::BuildIndex(const std::filesystem::path& dataPath,
                                            const std::filesystem::path& requestedIndexPath,
                                            std::size_t stride) noexcept {
    if (dataPath.empty() || stride == 0) return false;
    try {
        const auto absoluteData = Absolute(dataPath);
        const auto indexPath = requestedIndexPath.empty() ? IndexPath(absoluteData)
                                                           : Absolute(requestedIndexPath);
        std::ifstream input(absoluteData, std::ios::binary);
        if (!input || indexPath.empty()) return false;
        if (indexPath.has_parent_path()) std::filesystem::create_directories(indexPath.parent_path());
        std::ofstream output(indexPath, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << kIndexHeader << '\n';
        std::string line;
        std::size_t row = 0;
        std::string previous;
        while (true) {
            const auto offset = input.tellg();
            if (!std::getline(input, line)) break;
            DictionaryEntry entry;
            std::string key;
            if (!Parse(line, entry, key)) continue;
            if (!previous.empty() && key < previous) return false;
            previous = key;
            if ((row++ % stride) == 0) output << static_cast<std::uint64_t>(offset) << '\t' << key << '\n';
        }
        return static_cast<bool>(output) && row != 0;
    } catch (...) {
        return false;
    }
}

bool ExternalDictionaryProvider::LoadIndex() noexcept {
    try {
        std::ifstream input(indexPath_, std::ios::binary);
        if (!input) return false;
        std::string line;
        if (!std::getline(input, line) || Trim(line) != kIndexHeader) return false;
        std::vector<IndexRow> rows;
        std::uint64_t previousOffset = 0;
        std::string previousKey;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) return false;
            const auto offset = std::stoull(line.substr(0, separator));
            const auto key = line.substr(separator + 1);
            if (key.empty() || (!rows.empty() && (offset <= previousOffset || key < previousKey))) {
                return false;
            }
            rows.push_back({key, offset});
            previousOffset = offset;
            previousKey = key;
        }
        if (rows.empty()) return false;
        index_ = std::move(rows);
        return true;
    } catch (...) {
        index_.clear();
        return false;
    }
}

std::optional<DictionaryEntry> ExternalDictionaryProvider::Find(
    std::wstring_view entryId) const {
    if (!loaded_) return std::nullopt;
    const auto key = ToAscii(entryId);
    if (key.empty()) return std::nullopt;
    const auto it = std::upper_bound(index_.begin(), index_.end(), key,
                                     [](std::string_view value, const IndexRow& row) {
                                         return value < row.key;
                                     });
    const auto start = it == index_.begin() ? index_.begin() : std::prev(it);
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) return std::nullopt;
        input.seekg(static_cast<std::streamoff>(start->offset));
        std::string line;
        while (std::getline(input, line)) {
            DictionaryEntry entry;
            std::string rowKey;
            if (!Parse(line, entry, rowKey)) continue;
            if (rowKey == key) return entry;
            if (rowKey > key) break;
        }
    } catch (...) {
    }
    return std::nullopt;
}

ExternalPronunciationProvider::ExternalPronunciationProvider(std::filesystem::path path)
    : path_(Absolute(std::move(path), ExternalDictionaryProvider::DataPackRoot())) {
    if (path_.empty()) return;
    indexPath_ = path_;
    indexPath_ += L".idx";
    std::ifstream input(path_, std::ios::binary);
    loaded_ = static_cast<bool>(input) && LoadIndex();
}

std::filesystem::path ExternalPronunciationProvider::DefaultPath() {
    const auto root = ExternalDictionaryProvider::DataPackRoot();
    return root.empty() ? std::filesystem::path{}
                        : root / L"pronunciation" / L"pronunciations.tsv";
}

bool ExternalPronunciationProvider::LoadIndex() noexcept {
    try {
        std::ifstream input(indexPath_, std::ios::binary);
        std::string line;
        if (!input || !std::getline(input, line) || Trim(line) != kPronunciationIndexHeader) {
            return false;
        }
        std::vector<IndexRow> rows;
        std::uint64_t previousOffset = 0;
        std::string previousKey;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) return false;
            const auto offset = std::stoull(line.substr(0, separator));
            const auto key = line.substr(separator + 1);
            if (key.empty() || (!rows.empty() && (offset <= previousOffset || key < previousKey))) {
                return false;
            }
            rows.push_back({key, offset});
            previousOffset = offset;
            previousKey = key;
        }
        if (rows.empty()) return false;
        index_ = std::move(rows);
        return true;
    } catch (...) {
        index_.clear();
        return false;
    }
}

std::wstring ExternalPronunciationProvider::Find(std::wstring_view headword) const {
    if (!loaded_) return {};
    auto key = ToAscii(headword);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (key.empty()) return {};
    const auto it = std::upper_bound(index_.begin(), index_.end(), key,
                                     [](std::string_view value, const IndexRow& row) {
                                         return value < row.key;
                                     });
    const auto start = it == index_.begin() ? index_.begin() : std::prev(it);
    try {
        std::ifstream input(path_, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(start->offset));
        std::string line;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) continue;
            const auto rowKey = line.substr(0, separator);
            if (rowKey == key) return ToWide(Trim(line.substr(separator + 1)));
            if (rowKey > key) break;
        }
    } catch (...) {
    }
    return {};
}

}  // namespace tekito::dictionary
