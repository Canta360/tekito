#include "Core/ExternalRankingProviders.h"

#include "Core/ExternalLexiconProvider.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <string_view>

namespace tekito {
namespace {

constexpr std::string_view kFrequencyHeader = "TEKITO_FREQUENCY_INDEX_V1";
constexpr std::string_view kPhraseHeader = "TEKITO_PHRASE_INDEX_V1";

std::string ToAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch < 0 || ch > 0x7f) return {};
        result.push_back(static_cast<char>(std::towlower(ch)));
    }
    return result;
}

std::filesystem::path Absolute(std::filesystem::path path) {
    if (path.empty()) return {};
    if (path.is_relative()) path = ExternalLexiconProvider::DataPackRoot() / path;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return error ? std::filesystem::path{} : absolute;
}

std::string KeyFor(std::string_view first, std::string_view second = {}) {
    return second.empty() ? std::string(first) : std::string(first) + '\t' + std::string(second);
}

bool BuildIndexFile(const std::filesystem::path& dataPath,
                    const std::filesystem::path& requestedIndexPath,
                    std::string_view header, std::size_t stride,
                    bool phrase) noexcept {
    if (dataPath.empty() || stride == 0) return false;
    try {
        const auto data = Absolute(dataPath);
        auto index = requestedIndexPath.empty() ? std::filesystem::path{} : Absolute(requestedIndexPath);
        if (index.empty() && !data.empty()) {
            index = data;
            index += L".idx";
        }
        std::ifstream input(data, std::ios::binary);
        if (!input || index.empty()) return false;
        if (index.has_parent_path()) std::filesystem::create_directories(index.parent_path());
        std::ofstream output(index, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << header << '\n';
        std::string line;
        std::size_t row = 0;
        std::string previous;
        while (true) {
            const auto offset = input.tellg();
            if (!std::getline(input, line)) break;
            if (line.empty() || line.front() == '#') continue;
            const auto firstEnd = line.find('\t');
            const auto secondEnd = phrase ? line.find('\t', firstEnd + 1) : std::string::npos;
            if (firstEnd == std::string::npos || (phrase && secondEnd == std::string::npos)) continue;
            const auto first = line.substr(0, firstEnd);
            const auto second = phrase ? line.substr(firstEnd + 1, secondEnd - firstEnd - 1) : std::string{};
            const auto key = KeyFor(first, second);
            if (!previous.empty() && key < previous) return false;
            previous = key;
            if ((row++ % stride) == 0) output << static_cast<std::uint64_t>(offset) << '\t' << key << '\n';
        }
        return static_cast<bool>(output) && row != 0;
    } catch (...) {
        return false;
    }
}

bool LoadIndexFile(const std::filesystem::path& path, std::string_view header,
                   std::vector<std::pair<std::string, std::uint64_t>>& rows) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        std::string line;
        if (!input || !std::getline(input, line) || line != header) return false;
        std::uint64_t previousOffset = 0;
        std::string previousKey;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) return false;
            const auto offset = std::stoull(line.substr(0, separator));
            const auto key = line.substr(separator + 1);
            if (key.empty() || (!rows.empty() && (offset <= previousOffset || key < previousKey))) return false;
            rows.push_back({key, offset});
            previousOffset = offset;
            previousKey = key;
        }
        return !rows.empty();
    } catch (...) {
        rows.clear();
        return false;
    }
}

template <typename IndexRow>
std::vector<IndexRow> ConvertRows(const std::vector<std::pair<std::string, std::uint64_t>>& rows) {
    std::vector<IndexRow> result;
    result.reserve(rows.size());
    for (const auto& row : rows) result.push_back({row.first, row.second});
    return result;
}

template <typename IndexRow>
const IndexRow* StartAt(const std::vector<IndexRow>& index, std::string_view key) {
    const auto it = std::upper_bound(index.begin(), index.end(), key,
                                     [](std::string_view value, const IndexRow& row) {
                                         return value < row.key;
                                     });
    return it == index.begin() ? &index.front() : &*std::prev(it);
}

struct ContextScoreEntry {
    std::string word;
    double score{0.0};
};

template <typename IndexRow>
std::vector<ContextScoreEntry> ReadContextBucket(std::ifstream& input,
                                                  const std::vector<IndexRow>& index,
                                                  std::string_view context) noexcept {
    std::vector<ContextScoreEntry> entries;
    if (context.empty() || index.empty() || !input.is_open()) return entries;
    try {
        input.clear();
        input.seekg(static_cast<std::streamoff>(StartAt(index, context)->offset), std::ios::beg);
        std::string line;
        while (std::getline(input, line)) {
            const auto firstEnd = line.find('\t');
            const auto secondEnd = line.find('\t', firstEnd + 1);
            if (firstEnd == std::string::npos || secondEnd == std::string::npos) continue;
            const auto rowContext = std::string_view(line).substr(0, firstEnd);
            if (rowContext < context) continue;
            if (rowContext > context) break;
            entries.push_back({line.substr(firstEnd + 1, secondEnd - firstEnd - 1),
                               std::stod(line.substr(secondEnd + 1))});
        }
    } catch (...) {
        entries.clear();
    }
    return entries;
}

template <typename Entry>
double ScoreFromContextBucket(const std::vector<Entry>& entries, std::string_view word) noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), word,
                                     [](const Entry& entry,
                                        std::string_view value) {
                                         return entry.word < value;
                                     });
    return it != entries.end() && it->word == word ? it->score : 0.0;
}

template <typename Entry>
std::size_t ContextBucketBytes(std::string_view context, const std::vector<Entry>& entries) noexcept {
    std::size_t bytes = context.size() + sizeof(Entry) + 48;
    for (const auto& entry : entries) bytes += entry.word.size() + sizeof(Entry) + 24;
    return bytes;
}

}  // namespace

ExternalFrequencyProvider::ExternalFrequencyProvider(std::filesystem::path path)
    : path_(Absolute(std::move(path))) {
    loaded_ = !path_.empty() && table_.Open(path_);
}

std::filesystem::path ExternalFrequencyProvider::DefaultPath() {
    const auto root = ExternalLexiconProvider::DataPackRoot();
    return root.empty() ? std::filesystem::path{} : root / L"frequency" / L"word-scores.tsv";
}

std::filesystem::path ExternalFrequencyProvider::IndexPath(const std::filesystem::path& dataPath) {
    if (dataPath.empty()) return {};
    auto result = dataPath;
    result += L".idx";
    return result;
}

bool ExternalFrequencyProvider::BuildIndex(const std::filesystem::path& dataPath,
                                           const std::filesystem::path& indexPath,
                                           std::size_t stride) noexcept {
    return BuildIndexFile(dataPath, indexPath, kFrequencyHeader, stride, false);
}

double ExternalFrequencyProvider::Score(std::wstring_view word) const noexcept {
    if (!loaded_) return 0.0;
    double score = 0.0;
    try {
        table_.ForEachRow(ToAscii(word), [&](const std::vector<std::string_view>& fields) {
            if (fields.size() >= 2) std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), score);
            return false;
        });
    } catch (...) {
        return 0.0;
    }
    return score;
}

ExternalPhraseContextProvider::ExternalPhraseContextProvider(std::filesystem::path path)
    : path_(Absolute(std::move(path))), indexPath_(RuntimeIndexPath(path_)) {
    std::vector<std::pair<std::string, std::uint64_t>> rows;
    dataFile_.open(path_, std::ios::binary);
    loaded_ = !path_.empty() && static_cast<bool>(dataFile_) &&
              LoadIndexFile(indexPath_, kPhraseHeader, rows);
    if (loaded_) index_ = ConvertRows<IndexRow>(rows);
}

std::filesystem::path ExternalPhraseContextProvider::RuntimeIndexPath(
    const std::filesystem::path& dataPath) {
    auto dense = ExternalPhraseContextProvider::IndexPath(dataPath);
    dense += L".stride64";
    std::error_code error;
    if (std::filesystem::exists(dense, error) && !error) return dense;
    return ExternalPhraseContextProvider::IndexPath(dataPath);
}

std::filesystem::path ExternalPhraseContextProvider::DefaultPath() {
    const auto root = ExternalLexiconProvider::DataPackRoot();
    return root.empty() ? std::filesystem::path{} : root / L"phrase" / L"context-scores.tsv";
}

std::filesystem::path ExternalPhraseContextProvider::IndexPath(const std::filesystem::path& dataPath) {
    if (dataPath.empty()) return {};
    auto result = dataPath;
    result += L".idx";
    return result;
}

bool ExternalPhraseContextProvider::BuildIndex(const std::filesystem::path& dataPath,
                                                const std::filesystem::path& indexPath,
                                                std::size_t stride) noexcept {
    return BuildIndexFile(dataPath, indexPath, kPhraseHeader, stride, true);
}

double ExternalPhraseContextProvider::Score(std::wstring_view word,
                                             std::wstring_view context) const noexcept {
    constexpr std::size_t kContextCacheBudgetBytes = 32u * 1024u * 1024u;
    const auto asciiContext = ToAscii(context);
    const auto asciiWord = ToAscii(word);
    if (asciiContext.empty() || asciiWord.empty()) return 0.0;
    std::lock_guard lock(dataFileMutex_);
    if (!loaded_) return 0.0;

    if (const auto cached = contextCache_.find(asciiContext); cached != contextCache_.end()) {
        contextLru_.splice(contextLru_.begin(), contextLru_, cached->second.lruPosition);
        return ScoreFromContextBucket(cached->second.entries, asciiWord);
    }

    auto loaded = ReadContextBucket(dataFile_, index_, asciiContext);
    const auto score = ScoreFromContextBucket(loaded, asciiWord);
    const auto bytes = ContextBucketBytes(asciiContext, loaded);
    if (bytes > kContextCacheBudgetBytes) return score;

    while (contextCacheBytes_ + bytes > kContextCacheBudgetBytes && !contextLru_.empty()) {
        const auto& oldestKey = contextLru_.back();
        const auto entry = contextCache_.find(oldestKey);
        if (entry != contextCache_.end()) {
            contextCacheBytes_ -= entry->second.bytes;
            contextCache_.erase(entry);
        }
        contextLru_.pop_back();
    }
    contextLru_.push_front(asciiContext);
    CachedContextBucket bucket;
    bucket.entries.reserve(loaded.size());
    for (auto& entry : loaded) bucket.entries.push_back({std::move(entry.word), entry.score});
    bucket.bytes = bytes;
    bucket.lruPosition = contextLru_.begin();
    contextCacheBytes_ += bytes;
    contextCache_.emplace(asciiContext, std::move(bucket));
    return score;
}

}  // namespace tekito
