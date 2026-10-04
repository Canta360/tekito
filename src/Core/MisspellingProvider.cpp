#include "Core/MisspellingProvider.h"

#include "Core/DataPackPath.h"
#include "Core/ExternalLexiconProvider.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

namespace tekito {
namespace {

constexpr std::string_view kIndexHeader = "TEKITO_WIKIPEDIA_MISSPELLING_INDEX_V1";

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

bool IsAsciiWord(std::string_view value) {
    if (value.empty()) return false;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '\'' || ch == '-') {
            continue;
        }
        return false;
    }
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalpha(ch) != 0;
    });
}

std::string LowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::wstring WidenAscii(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char ch : value) result.push_back(static_cast<wchar_t>(ch));
    return result;
}

std::string NarrowAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch < 0 || ch > 0x7f) return {};
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool ParseLine(std::string_view line, std::string& misspelling, std::string& correction) {
    line = Trim(line);
    if (line.empty() || line.front() == '#') return false;
    const auto separator = line.find('\t');
    if (separator == std::string_view::npos) return false;
    const auto raw = Trim(line.substr(0, separator));
    const auto fixed = Trim(line.substr(separator + 1));
    if (!IsAsciiWord(raw) || !IsAsciiWord(fixed)) return false;
    misspelling = LowerAscii(raw);
    correction.assign(fixed);
    return true;
}

}  // namespace

WikipediaCommonMisspellingsProvider::WikipediaCommonMisspellingsProvider(
    std::filesystem::path path)
    : path_(std::move(path)), indexPath_(IndexPath(path_)) {
    if (path_.empty()) return;
    if (path_.is_relative()) {
        const auto root = ExternalLexiconProvider::DataPackRoot();
        if (!root.empty()) path_ = root / path_;
    }
    std::error_code error;
    path_ = std::filesystem::absolute(path_, error);
    if (error || path_.empty()) return;
    indexPath_ = IndexPath(path_);
    dataFile_.open(path_, std::ios::binary);
    loaded_ = static_cast<bool>(dataFile_) && LoadIndex();
}

std::filesystem::path WikipediaCommonMisspellingsProvider::DefaultPath() {
    const auto root = ExternalLexiconProvider::DataPackRoot();
    if (root.empty()) return {};
    return FindDataPack(root, L"wikipedia-common-misspellings") / L"misspellings.tsv";
}

std::filesystem::path WikipediaCommonMisspellingsProvider::IndexPath(
    const std::filesystem::path& packPath) {
    if (packPath.empty()) return {};
    auto result = packPath;
    result += L".idx";
    return result;
}

bool WikipediaCommonMisspellingsProvider::BuildIndex(
    const std::filesystem::path& packPath, const std::filesystem::path& requestedIndexPath,
    std::size_t stride) noexcept {
    if (packPath.empty() || stride == 0) return false;
    try {
        std::error_code error;
        const auto inputPath = std::filesystem::absolute(packPath, error);
        const auto outputPath = requestedIndexPath.empty()
                                    ? IndexPath(inputPath)
                                    : std::filesystem::absolute(requestedIndexPath, error);
        if (error || inputPath.empty() || outputPath.empty()) return false;

        std::ifstream input(inputPath, std::ios::binary);
        if (!input) return false;
        if (outputPath.has_parent_path()) {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) return false;

        output << kIndexHeader << '\n';
        std::string line;
        std::string previousKey;
        std::size_t rowNumber = 0;
        while (true) {
            const auto offset = input.tellg();
            if (!std::getline(input, line)) break;
            std::string misspelling;
            std::string correction;
            if (!ParseLine(line, misspelling, correction)) continue;
            if (!previousKey.empty() && misspelling < previousKey) return false;
            previousKey = misspelling;
            if ((rowNumber++ % stride) == 0) {
                output << static_cast<std::uint64_t>(offset) << '\t' << misspelling << '\n';
            }
        }
        return static_cast<bool>(output) && rowNumber != 0;
    } catch (...) {
        return false;
    }
}

bool WikipediaCommonMisspellingsProvider::LoadIndex() noexcept {
    try {
        std::ifstream input(indexPath_, std::ios::binary);
        if (!input) return false;
        std::string line;
        if (!std::getline(input, line) || Trim(line) != kIndexHeader) return false;

        std::vector<IndexRow> rows;
        std::string previousKey;
        std::uint64_t previousOffset = 0;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) return false;
            const auto offset = std::stoull(line.substr(0, separator));
            const auto key = line.substr(separator + 1);
            if (!IsAsciiWord(key) || (!rows.empty() &&
                                      (offset <= previousOffset || key < previousKey))) {
                return false;
            }
            rows.push_back({key, offset});
            previousKey = key;
            previousOffset = offset;
        }
        if (rows.empty()) return false;
        index_ = std::move(rows);
        return true;
    } catch (...) {
        index_.clear();
        return false;
    }
}

void WikipediaCommonMisspellingsProvider::Visit(const Visitor& visitor) const {
    try {
        std::lock_guard lock(dataFileMutex_);
        if (!dataFile_.is_open()) return;
        dataFile_.clear();
        dataFile_.seekg(0);
        std::string line;
        while (std::getline(dataFile_, line)) {
            std::string misspelling;
            std::string correction;
            if (!ParseLine(line, misspelling, correction)) continue;
            const auto raw = WidenAscii(misspelling);
            const auto fixed = WidenAscii(correction);
            visitor({raw, fixed});
        }
    } catch (...) {
    }
}

void WikipediaCommonMisspellingsProvider::Find(std::wstring_view misspelling,
                                                const Visitor& visitor) const {
    if (!loaded_) return;
    const auto key = NarrowAscii(misspelling);
    if (key.empty()) return;

    const auto it = std::upper_bound(
        index_.begin(), index_.end(), key,
        [](std::string_view value, const IndexRow& row) { return value < row.firstKey; });
    const auto start = it == index_.begin() ? index_.begin() : std::prev(it);

    try {
        std::lock_guard lock(dataFileMutex_);
        if (!dataFile_.is_open()) return;
        dataFile_.clear();
        dataFile_.seekg(static_cast<std::streamoff>(start->offset), std::ios::beg);
        if (!dataFile_) return;

        std::string line;
        while (std::getline(dataFile_, line)) {
            std::string raw;
            std::string fixed;
            if (!ParseLine(line, raw, fixed)) continue;
            if (raw == key) {
                const auto rawText = WidenAscii(raw);
                const auto fixedText = WidenAscii(fixed);
                visitor({rawText, fixedText});
                continue;
            }
            if (raw > key) return;
        }
    } catch (...) {
    }
}

}  // namespace tekito
