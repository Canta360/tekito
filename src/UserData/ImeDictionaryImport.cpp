#include "UserData/ImeDictionaryImport.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace tekito::userdata {
namespace {

using japanese::UserWord;
using japanese::UserWordAction;
using japanese::UserWordKind;

std::wstring Decode(std::string_view bytes, UINT codePage, DWORD flags) {
    if (bytes.empty()) return {};
    const int length = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    return text;
}

// The file's text, whichever way the input method wrote it.
std::wstring DecodeText(std::string_view bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        for (std::size_t i = 0; i < text.size(); ++i) {
            text[i] = static_cast<wchar_t>(static_cast<unsigned char>(bytes[2 + 2 * i]) |
                                           (static_cast<unsigned char>(bytes[3 + 2 * i]) << 8));
        }
        return text;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        for (std::size_t i = 0; i < text.size(); ++i) {
            text[i] = static_cast<wchar_t>((static_cast<unsigned char>(bytes[2 + 2 * i]) << 8) |
                                           static_cast<unsigned char>(bytes[3 + 2 * i]));
        }
        return text;
    }
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") bytes.remove_prefix(3);
    // UTF-8 if it is valid UTF-8; older exports are Shift_JIS.
    if (auto utf8 = Decode(bytes, CP_UTF8, MB_ERR_INVALID_CHARS); !utf8.empty()) return utf8;
    return Decode(bytes, 932, 0);
}

// A reading in hiragana (katakana becomes hiragana); nothing if it is not kana.
std::optional<std::wstring> Reading(std::wstring_view text) {
    std::wstring reading;
    for (wchar_t c : text) {
        if (c >= 0x30A1 && c <= 0x30F6) c = static_cast<wchar_t>(c - 0x60);
        const bool kana = (c >= 0x3041 && c <= 0x3096) || c == 0x30FC || c == 0x309D || c == 0x309E;
        if (!kana) return std::nullopt;
        reading.push_back(c);
    }
    if (reading.empty()) return std::nullopt;
    return reading;
}

bool Contains(std::wstring_view text, std::wstring_view part) { return text.find(part) != std::wstring_view::npos; }

// The kind for another input method's part of speech, by the words they
// use for it (person name, place name, a lone given-name mark ...); a noun
// otherwise.
UserWordKind KindFor(std::wstring_view pos) {
    // ATOK marks its parts of speech with "*".
    while (!pos.empty() && (pos.back() == L'*' || pos.back() == L' ')) pos.remove_suffix(1);
    if (pos == L"\u59D3") return UserWordKind::Surname;
    if (pos == L"\u540D") return UserWordKind::GivenName;
    if (Contains(pos, L"\u4EBA\u540D")) return UserWordKind::Person;
    if (Contains(pos, L"\u5730\u540D")) return UserWordKind::Place;
    if (Contains(pos, L"\u7D44\u7E54") || Contains(pos, L"\u4F1A\u793E")) return UserWordKind::Organization;
    if (Contains(pos, L"\u56FA\u6709")) return UserWordKind::ProperNoun;
    if (Contains(pos, L"\u30B5\u5909") || Contains(pos, L"\u3055\u5909")) return UserWordKind::SuruNoun;
    if (Contains(pos, L"\u9854\u6587\u5B57") || Contains(pos, L"\u8A18\u53F7")) return UserWordKind::Symbol;
    if (Contains(pos, L"\u611F\u52D5\u8A5E") || Contains(pos, L"\u3042\u3044\u3055\u3064") ||
        Contains(pos, L"\u6328\u62F6")) {
        return UserWordKind::Interjection;
    }
    return UserWordKind::Noun;
}

UserWordAction ActionFor(std::wstring_view pos) {
    if (Contains(pos, L"\u6291\u5236")) return UserWordAction::Suppress;
    if (Contains(pos, L"\u30B5\u30B8\u30A7\u30B9\u30C8\u306E\u307F")) return UserWordAction::Suggest;
    return UserWordAction::First;
}

}  // namespace

std::optional<ImportedImeWords> ParseImeDictionary(std::string_view bytes) {
    const std::wstring text = DecodeText(bytes);
    ImportedImeWords result;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring_view line(text.data() + at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        if (line.empty() || line.front() == L'!' || line.front() == L'#') continue;
        std::vector<std::wstring_view> fields;
        for (std::size_t start = 0;;) {
            const std::size_t tab = line.find(L'\t', start);
            fields.push_back(line.substr(start, tab == std::wstring_view::npos ? std::wstring_view::npos : tab - start));
            if (tab == std::wstring_view::npos) break;
            start = tab + 1;
        }
        const auto reading = fields.size() >= 2 ? Reading(fields[0]) : std::nullopt;
        if (!reading || fields[1].empty()) {
            ++result.skipped;
            continue;
        }
        const std::wstring_view pos = fields.size() >= 3 ? fields[2] : std::wstring_view{};
        result.words.push_back({*reading, std::wstring(fields[1]), KindFor(pos), ActionFor(pos)});
    }
    if (result.words.empty()) return std::nullopt;
    return result;
}

std::optional<ImportedImeWords> ReadImeDictionary(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::nullopt;
    // A user dictionary is small; anything huge is some other file.
    constexpr std::streamoff kLargest = 64ll * 1024 * 1024;
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0 || size > kLargest) return std::nullopt;
    stream.seekg(0);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!stream.read(bytes.data(), size)) return std::nullopt;
    return ParseImeDictionary(bytes);
}

std::size_t MergeImeWords(std::vector<UserWord>& words, const std::vector<UserWord>& imported) {
    std::size_t added = 0;
    for (const auto& word : imported) {
        const bool known = std::any_of(words.begin(), words.end(), [&](const UserWord& w) {
            return w.reading == word.reading && w.surface == word.surface;
        });
        if (known) continue;
        words.push_back(word);
        ++added;
    }
    return added;
}

}  // namespace tekito::userdata
