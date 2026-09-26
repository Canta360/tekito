#include "Core/ExternalSlangProvider.h"

#include "Core/ExternalLexiconProvider.h"

#include <algorithm>
#include <cctype>
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#endif
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#include <locale>
#endif
#include <string>
#include <string_view>
#include <utility>

namespace tekito {
namespace {

constexpr std::string_view kIndexHeader = "TEKITO_SLANG_INDEX_V1";

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

std::filesystem::path Absolute(std::filesystem::path path,
                               const std::filesystem::path& base) {
    if (path.empty()) return {};
    if (path.is_relative() && !base.empty()) path = base / path;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return error ? std::filesystem::path{} : absolute;
}

std::wstring Wide(std::string_view text) {
#if defined(_WIN32)
    if (text.size() <= static_cast<std::size_t>(INT_MAX)) {
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0);
        if (length > 0) {
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), result.data(), length) == length) {
                return result;
            }
        }
    }
#else
    try {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.from_bytes(text.data(), text.data() + text.size());
    } catch (...) {
    }
#endif
    std::wstring result;
    result.reserve(text.size());
    for (const unsigned char ch : text) result.push_back(static_cast<wchar_t>(ch));
    return result;
}

std::string Lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

std::string ToAscii(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        if (ch < 0 || ch > 0x7f) return {};
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool Split(std::string_view line, std::string_view fields[6]) {
    if (line.empty() || line.front() == '#') return false;
    for (std::size_t index = 0; index < 6; ++index) {
        const auto separator = line.find('\t');
        fields[index] = Trim(separator == std::string_view::npos ? line : line.substr(0, separator));
        line = separator == std::string_view::npos ? std::string_view{} : line.substr(separator + 1);
    }
    return !fields[0].empty() && fields[0] != "raw" && !fields[1].empty();
}

SemanticLabel Label(std::string_view value) {
    if (value == "abbreviation") return SemanticLabel::Abbreviation;
    if (value == "phrase" || value == "colloquial") return SemanticLabel::Phrase;
    if (value == "proper") return SemanticLabel::ProperNoun;
    if (value == "emoji") return SemanticLabel::Emoji;
    if (value == "social" || value == "phonetic") return SemanticLabel::Reaction;
    return SemanticLabel::Slang;
}

std::uint8_t SocialRange(std::string_view tier, std::string_view label,
                         std::string_view notes,
                         std::uint32_t sourceFlags) {
    if (tier == "common" || notes.starts_with("common\t")) return SocialRangeCommon;
    if (tier == "familiar" || notes.starts_with("familiar\t")) return SocialRangeFamiliar;
    if (tier == "broad" || notes.starts_with("broad\t")) return SocialRangeBroad;
    if (tier == "full" || notes.starts_with("full\t")) return SocialRangeFull;
    if ((sourceFlags & CandidateSourceTekitoOwnedSlang) != 0) return SocialRangeCommon;
    if ((sourceFlags & CandidateSourceWiktionary) != 0) return SocialRangeFull;
    if ((sourceFlags & CandidateSourceEmoji) != 0 || label == "emoji") return SocialRangeBroad;
    return SocialRangeUnspecified;
}

void Emit(std::string_view line, std::uint32_t sourceFlags,
          const ILexiconProvider::Visitor& visitor) {
    std::string_view fields[6];
    if (!Split(line, fields)) return;
    const auto raw = Wide(fields[0]);
    const auto candidate = Wide(fields[1]);
    const auto label = Label(fields[2]);
    const bool proper = label == SemanticLabel::ProperNoun;
    const bool emoji = label == SemanticLabel::Emoji;
    const bool suggestOnly = fields[2] == "social" || fields[2] == "phonetic";
    const auto socialRange = SocialRange(fields[4], fields[2], fields[5], sourceFlags);
    const auto rawLabel = emoji ? SemanticLabel::Original : label;
    const auto candidateLabel = proper || emoji ? label : SemanticLabel::Standard;
    const auto rawPolicy = emoji ? CandidatePolicyNone : CandidatePolicyProtect;
    const auto candidatePolicy = emoji ? CandidatePolicyEmoji | CandidatePolicySuggestOnly
                                       : proper || suggestOnly ? CandidatePolicySuggestOnly
                                                                 : CandidatePolicyExpand | CandidatePolicySuggestOnly;
    visitor({raw, candidate, rawLabel, candidateLabel, true, false, rawPolicy,
             candidatePolicy, sourceFlags, false, socialRange});
}

}  // namespace

ExternalSlangProvider::ExternalSlangProvider(std::filesystem::path path,
                                             std::uint32_t sourceFlags)
    : path_(Absolute(std::move(path), ExternalLexiconProvider::DataPackRoot())),
      sourceFlags_(sourceFlags) {
    if (path_.empty()) return;
    indexPath_ = path_;
    indexPath_ += L".idx";
    dataFile_.open(path_, std::ios::binary);
    loaded_ = static_cast<bool>(dataFile_) && LoadIndex();
}

std::filesystem::path ExternalSlangProvider::DefaultPath() {
    const auto root = ExternalLexiconProvider::DataPackRoot();
    return root.empty() ? std::filesystem::path{} : root / L"slang" / L"entries.tsv";
}

std::span<const LexiconEntry> ExternalSlangProvider::Entries() const noexcept {
    return {};
}

void ExternalSlangProvider::Visit(const Visitor& visitor) const {
    try {
        std::lock_guard lock(dataFileMutex_);
        if (!dataFile_.is_open()) return;
        dataFile_.clear();
        dataFile_.seekg(0);
        std::string line;
        while (std::getline(dataFile_, line)) Emit(line, sourceFlags_, visitor);
    } catch (...) {
    }
}

bool ExternalSlangProvider::LoadIndex() noexcept {
    try {
        std::ifstream input(indexPath_, std::ios::binary);
        if (!input) return false;
        std::string line;
        if (!std::getline(input, line) || Trim(line) != kIndexHeader) return false;
        std::vector<IndexRow> rows;
        std::uint64_t previousOffset = 0;
        std::string previousRaw;
        while (std::getline(input, line)) {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) return false;
            const auto offset = std::stoull(line.substr(0, separator));
            const auto raw = Lower(Trim(line.substr(separator + 1)));
            if (raw.empty() || (!rows.empty() && (offset <= previousOffset || raw < previousRaw))) return false;
            rows.push_back({raw, offset});
            previousOffset = offset;
            previousRaw = raw;
        }
        if (rows.empty()) return false;
        index_ = std::move(rows);
        return true;
    } catch (...) {
        index_.clear();
        return false;
    }
}

void ExternalSlangProvider::Find(const LexiconQuery& query, const Visitor& visitor) const {
    if (!loaded_ || query.text.empty() || query.kind == LexiconQuery::Kind::Candidate) return;
    const auto key = ToAscii(query.text);
    if (key.empty()) return;
    const auto it = std::upper_bound(index_.begin(), index_.end(), key,
                                     [](std::string_view value, const IndexRow& row) {
                                         return value < row.raw;
                                     });
    const auto start = it == index_.begin() ? index_.begin() : std::prev(it);
    std::lock_guard lock(dataFileMutex_);
    if (!dataFile_.is_open()) return;
    dataFile_.clear();
    dataFile_.seekg(static_cast<std::streamoff>(start->offset), std::ios::beg);
    if (!dataFile_) return;
    std::string line;
    std::size_t resultCount = 0;
    while (std::getline(dataFile_, line)) {
        std::string_view fields[6];
        if (!Split(line, fields)) continue;
        const auto raw = Lower(fields[0]);
        const bool match = query.kind == LexiconQuery::Kind::Exact ? raw == key
                                                                     : raw.starts_with(key);
        if (match) {
            Emit(line, sourceFlags_, visitor);
            if (query.maxResults != 0 && ++resultCount >= query.maxResults) return;
        }
        if (query.kind == LexiconQuery::Kind::Exact && raw > key) return;
        if (query.kind == LexiconQuery::Kind::Prefix && !raw.starts_with(key) && raw > key) return;
    }
}

}  // namespace tekito
