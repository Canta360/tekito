#include "Core/ExternalLexiconProvider.h"

#include "Core/DataPackPath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <userenv.h>
#pragma comment(lib, "userenv.lib")
#endif

namespace tekito {
namespace {

constexpr std::string_view kIndexHeader = "TEKITO_LEXICON_INDEX_V1";

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

std::string_view FirstToken(std::string_view line) {
    line = Trim(line);
    if (line.empty() || line.front() == '#') return {};

    // ESDB scowl.txt stores the lemma after the metadata delimiter.
    if (const auto delimiter = line.find(": "); delimiter != std::string_view::npos) {
        line = Trim(line.substr(delimiter + 2));
    }
    if (const auto tab = line.find('\t'); tab != std::string_view::npos) {
        line = line.substr(0, tab);
    }
    if (const auto space = line.find_first_of(" \r\n\t"); space != std::string_view::npos) {
        line = line.substr(0, space);
    }
    return line;
}

bool IsWord(std::string_view token) {
    if (token.empty()) return false;
    for (const unsigned char ch : token) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            ch == '\'' || ch == '-') {
            continue;
        }
        return false;
    }
    return std::any_of(token.begin(), token.end(), [](unsigned char ch) {
        return std::isalpha(ch) != 0;
    });
}

std::wstring LowerAscii(std::string_view token) {
    std::wstring result;
    result.reserve(token.size());
    for (const unsigned char ch : token) {
        result.push_back(static_cast<wchar_t>(std::tolower(ch)));
    }
    return result;
}

std::string LowerAsciiString(std::string_view token) {
    std::string result(token);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string ToAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch < 0 || ch > 0x7f) return {};
        result.push_back(static_cast<char>(ch));
    }
    return LowerAsciiString(result);
}

std::filesystem::path AbsoluteFrom(std::filesystem::path path,
                                   const std::filesystem::path& relativeBase) {
    if (path.empty()) return {};
    if (path.is_relative() && !relativeBase.empty()) path = relativeBase / path;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return error ? std::filesystem::path{} : absolute;
}

std::filesystem::path WindowsEnvironmentPath(const wchar_t* name) {
#if defined(_WIN32)
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) == 0 && value && *value) {
        const std::filesystem::path path(value);
        std::free(value);
        return path;
    }
    std::free(value);
#else
    (void)name;
#endif
    return {};
}

#if defined(_WIN32)
// In an app container (the Start menu's search, Store apps) LOCALAPPDATA
// is the app's own folder: the user's is found from the profile instead.
std::filesystem::path AppContainerLocalAppData() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::filesystem::path result;
    DWORD inContainer = 0;
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIsAppContainer, &inContainer, sizeof(inContainer), &size) && inContainer) {
        wchar_t profile[MAX_PATH]{};
        DWORD length = MAX_PATH;
        if (GetUserProfileDirectoryW(token, profile, &length)) {
            result = std::filesystem::path(profile) / L"AppData" / L"Local";
        }
    }
    CloseHandle(token);
    return result;
}
#endif

std::filesystem::path ResolveDataPackRoot() {
#if defined(_WIN32)
    const auto configured = WindowsEnvironmentPath(L"TEKITO_DATA_PACK_DIR");
    if (!configured.empty()) return AbsoluteFrom(configured, {});
    static const auto user = AppContainerLocalAppData();
    if (!user.empty()) {
        return AbsoluteFrom(user / L"TEKITO" / L"data", {});
    }
    const auto localAppData = WindowsEnvironmentPath(L"LOCALAPPDATA");
    if (localAppData.empty()) return {};
    return AbsoluteFrom(localAppData / L"TEKITO" / L"data", {});
#else
    if (const auto* configured = std::getenv("TEKITO_DATA_PACK_DIR"); configured && *configured) {
        return AbsoluteFrom(configured, {});
    }
    if (const auto* home = std::getenv("HOME"); home && *home) {
        return AbsoluteFrom(std::filesystem::path(home) / ".local" / "share" / "TEKITO" /
                                "data",
                            {});
    }
    return {};
#endif
}

std::filesystem::path ReadConfiguredLexiconPath(const std::filesystem::path& root) {
#if defined(_WIN32)
    const auto configured = WindowsEnvironmentPath(L"TEKITO_LEXICON_PATH");
    if (!configured.empty()) return AbsoluteFrom(configured, root);
#else
    if (const auto* configured = std::getenv("TEKITO_LEXICON_PATH"); configured && *configured) {
        return AbsoluteFrom(configured, root);
    }
#endif
    return {};
}

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool Near(std::string_view left, std::string_view right, std::size_t limit) {
    if (left.size() > right.size() + limit || right.size() > left.size() + limit) return false;
    constexpr std::size_t kStackColumns = 128;
    std::array<std::size_t, kStackColumns> stackPrevious{};
    std::array<std::size_t, kStackColumns> stackCurrent{};
    std::vector<std::size_t> heapPrevious;
    std::vector<std::size_t> heapCurrent;
    auto* previous = stackPrevious.data();
    auto* current = stackCurrent.data();
    if (right.size() + 1 > kStackColumns) {
        heapPrevious.resize(right.size() + 1);
        heapCurrent.resize(right.size() + 1);
        previous = heapPrevious.data();
        current = heapCurrent.data();
    }
    for (std::size_t column = 0; column <= right.size(); ++column) previous[column] = column;
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        std::size_t best = current[0];
        for (std::size_t column = 1; column <= right.size(); ++column) {
            current[column] = std::min({current[column - 1] + 1, previous[column] + 1,
                                        previous[column - 1] +
                                            (left[row - 1] == right[column - 1] ? 0 : 1)});
            best = std::min(best, current[column]);
        }
        if (best > limit) return false;
        std::swap(previous, current);
    }
    return previous[right.size()] <= limit;
}

}  // namespace

ExternalLexiconProvider::ExternalLexiconProvider(std::filesystem::path path)
    : path_(AbsoluteFrom(std::move(path), ResolveDataPackRoot())), indexPath_(IndexPath(path_)) {
    if (path_.empty() || indexPath_.empty()) return;

    dataFile_.open(path_, std::ios::binary);
    loaded_ = static_cast<bool>(dataFile_) && LoadIndex();
    if (loaded_) BuildSymSpellIndex();
}

void ExternalLexiconProvider::BuildSymSpellIndex() noexcept {
    try {
        std::ifstream input(path_, std::ios::binary);
        if (!input) return;

        std::vector<std::string> words;
        std::string line;
        while (std::getline(input, line)) {
            const auto token = FirstToken(line);
            if (!IsWord(token)) continue;
            words.push_back(LowerAsciiString(token));
        }
        if (!words.empty()) symSpellIndex_.Build(std::move(words));
    } catch (...) {
        // A fuzzy index is an optimization; Kind::Candidate falls back to the
        // disk-streaming scan below when it is unavailable.
    }
}

std::filesystem::path ExternalLexiconProvider::DataPackRoot() {
    return ResolveDataPackRoot();
}

std::filesystem::path ExternalLexiconProvider::PackDirectory(std::wstring_view packId) {
    return FindDataPack(ResolveDataPackRoot(), packId);
}

std::filesystem::path ExternalLexiconProvider::DefaultPath() {
    const auto root = ResolveDataPackRoot();
    const auto configured = ReadConfiguredLexiconPath(root);
    if (!configured.empty()) return configured;
    if (root.empty()) return {};
    return FindDataPack(root, L"standard-english") / L"lexicon.txt";
}

std::filesystem::path ExternalLexiconProvider::IndexPath(
    const std::filesystem::path& wordListPath) {
    if (wordListPath.empty()) return {};
    auto result = wordListPath;
    result += L".idx";
    return result;
}

bool ExternalLexiconProvider::BuildIndex(const std::filesystem::path& wordListPath,
                                         const std::filesystem::path& requestedIndexPath,
                                         std::size_t stride) noexcept {
    if (wordListPath.empty() || stride == 0) return false;

    try {
        const auto absoluteWordList = AbsoluteFrom(wordListPath, {});
        const auto indexPath = requestedIndexPath.empty()
                                   ? IndexPath(absoluteWordList)
                                   : AbsoluteFrom(requestedIndexPath, {});
        if (absoluteWordList.empty() || indexPath.empty()) return false;

        std::ifstream input(absoluteWordList, std::ios::binary);
        if (!input) return false;
        if (indexPath.has_parent_path()) {
            std::filesystem::create_directories(indexPath.parent_path());
        }
        std::ofstream output(indexPath, std::ios::binary | std::ios::trunc);
        if (!output) return false;

        output << kIndexHeader << '\n';
        std::string line;
        std::size_t lineNumber = 0;
        std::string previousWord;
        while (true) {
            const auto offset = input.tellg();
            if (!std::getline(input, line)) break;
            const auto token = FirstToken(line);
            if (!IsWord(token)) continue;
            const auto word = LowerAsciiString(token);
            if (!previousWord.empty() && word < previousWord) return false;
            previousWord = word;
            if ((lineNumber++ % stride) == 0) {
                output << static_cast<std::uint64_t>(offset) << '\t' << word << '\n';
            }
        }
        return static_cast<bool>(output) && lineNumber != 0;
    } catch (...) {
        return false;
    }
}

bool ExternalLexiconProvider::LoadIndex() noexcept {
    try {
        std::ifstream input(indexPath_, std::ios::binary);
        if (!input) return false;

        std::string line;
        if (!std::getline(input, line) || Trim(line) != kIndexHeader) return false;

        std::vector<IndexRow> rows;
        std::uint64_t previousOffset = 0;
        std::string previousWord;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) return false;
            const auto offsetText = line.substr(0, separator);
            const auto word = line.substr(separator + 1);
            if (!IsWord(word) || word.empty()) return false;
            const auto offset = std::stoull(offsetText);
            if (!rows.empty() && (offset <= previousOffset || word < previousWord)) return false;
            rows.push_back({word, offset});
            previousOffset = offset;
            previousWord = word;
        }
        if (rows.empty()) return false;
        index_ = std::move(rows);
        return true;
    } catch (...) {
        index_.clear();
        return false;
    }
}

std::span<const LexiconEntry> ExternalLexiconProvider::Entries() const noexcept {
    return {};
}

void ExternalLexiconProvider::Visit(const Visitor& visitor) const {
    // Visit is intentionally retained for import/test tooling. Runtime
    // candidate generation uses Find(), which requires the local index.
    try {
        std::lock_guard lock(dataFileMutex_);
        if (!dataFile_.is_open()) return;
        dataFile_.clear();
        dataFile_.seekg(0);
        std::string line;
        while (std::getline(dataFile_, line)) {
            const auto token = FirstToken(line);
            if (!IsWord(token)) continue;
            const auto word = LowerAscii(token);
            const LexiconEntry entry{word, {}, SemanticLabel::Original,
                                     SemanticLabel::None, true, true,
                                     CandidatePolicyNone, CandidatePolicyNone,
                                     CandidateSourceExternalLexicon};
            visitor(entry);
        }
    } catch (...) {
        // An unreadable optional pack behaves like an empty import source.
    }
}

void ExternalLexiconProvider::Find(const LexiconQuery& query, const Visitor& visitor) const {
    if (!loaded_ || query.text.empty()) return;
    const auto key = ToAscii(query.text);
    if (key.empty()) return;

    // Below 3 characters, a 2-delete SymSpell query collides with a very large
    // share of the dictionary (most words reduce to some 0-1 character string
    // within 2 deletions), so the candidate pool it has to verify balloons and
    // latency with it. The disk-streaming scan below is bounded by same-first-
    // character prefix instead, which stays cheap at this length, so short
    // queries keep using it.
    if (query.kind == LexiconQuery::Kind::Candidate && symSpellIndex_.IsBuilt() && key.size() > 2) {
        auto matches = symSpellIndex_.Lookup(key, 2);
        std::size_t resultCount = 0;
        for (const auto& word : matches) {
            const auto ownedWord = LowerAscii(word);
            const LexiconEntry entry{ownedWord, {}, SemanticLabel::Original,
                                     SemanticLabel::None, true, true,
                                     CandidatePolicyNone, CandidatePolicyNone,
                                     CandidateSourceExternalLexicon};
            visitor(entry);
            ++resultCount;
            if (query.maxResults != 0 && resultCount >= query.maxResults) return;
        }
        return;
    }

    const auto it = std::upper_bound(
        index_.begin(), index_.end(), key,
        [](std::string_view value, const IndexRow& row) { return value < row.firstWord; });
    const auto start = it == index_.begin() ? index_.begin() : std::prev(it);

    try {
        std::lock_guard lock(dataFileMutex_);
        if (!dataFile_.is_open()) return;
        dataFile_.clear();
        dataFile_.seekg(static_cast<std::streamoff>(start->offset), std::ios::beg);
        if (!dataFile_) return;

        std::size_t resultCount = 0;
        std::string line;
        while (std::getline(dataFile_, line)) {
            const auto token = FirstToken(line);
            if (!IsWord(token)) continue;
            const auto word = LowerAsciiString(token);
            const bool match = query.kind == LexiconQuery::Kind::Exact
                                   ? word == key
                                   : query.kind == LexiconQuery::Kind::Candidate
                                         ? (!key.empty() && word.front() == key.front() && Near(word, key, 2))
                                         : StartsWith(word, key);
            if (match) {
                const auto ownedWord = LowerAscii(token);
                const LexiconEntry entry{ownedWord, {}, SemanticLabel::Original,
                                         SemanticLabel::None, true, true,
                                         CandidatePolicyNone, CandidatePolicyNone,
                                         CandidateSourceExternalLexicon};
                visitor(entry);
                ++resultCount;
                if (query.maxResults != 0 && resultCount >= query.maxResults) return;
            }
            if (query.kind == LexiconQuery::Kind::Exact && word > key) return;
            if (query.kind == LexiconQuery::Kind::Candidate &&
                (!key.empty() && word.front() > key.front())) return;
            if (query.kind == LexiconQuery::Kind::Prefix && word > key &&
                !StartsWith(word, key)) {
                return;
            }
        }
    } catch (...) {
        // An unreadable optional pack behaves like an empty query result.
    }
}

}  // namespace tekito
