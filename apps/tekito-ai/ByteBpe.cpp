#include "ByteBpe.h"

#include <icu.h>

#include <algorithm>
#include <fstream>
#include <limits>

namespace tekito::ai {

namespace {

void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Code points of UTF-8 text, each with where its bytes start.
struct CodePoints {
    std::vector<char32_t> cp;
    std::vector<std::size_t> at;
};

CodePoints Utf8Points(std::string_view s) {
    CodePoints out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::size_t n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        char32_t c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
        for (std::size_t k = 1; k < n && i + k < s.size(); ++k) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.cp.push_back(c);
        out.at.push_back(i);
        i += n;
    }
    out.at.push_back(s.size());
    return out;
}

bool IsLetter(char32_t c) {
    const auto t = u_charType(static_cast<UChar32>(c));
    return t == U_UPPERCASE_LETTER || t == U_LOWERCASE_LETTER || t == U_TITLECASE_LETTER || t == U_MODIFIER_LETTER ||
           t == U_OTHER_LETTER;
}
bool IsNumber(char32_t c) {
    const auto t = u_charType(static_cast<UChar32>(c));
    return t == U_DECIMAL_DIGIT_NUMBER || t == U_LETTER_NUMBER || t == U_OTHER_NUMBER;
}
bool IsSpace(char32_t c) { return u_hasBinaryProperty(static_cast<UChar32>(c), UCHAR_WHITE_SPACE) != 0; }
bool IsNewline(char32_t c) { return c == U'\r' || c == U'\n'; }

// Where each piece of the split ends, as the pack's tokenizer splits:
//   (?i:'s|'t|'re|'ve|'m|'ll|'d) | [^\r\n\p{L}\p{N}]?\p{L}+ | \p{N}{1,3}
//   | ' '?[^\s\p{L}\p{N}]+[\r\n]* | \s*[\r\n]+ | \s+(?!\S) | \s+
std::size_t PieceEnd(const std::vector<char32_t>& c, std::size_t i) {
    const std::size_t n = c.size();
    const auto lower = [&](std::size_t k) { return k < n ? static_cast<char32_t>(u_tolower(static_cast<UChar32>(c[k]))) : U'\0'; };
    if (c[i] == U'\'') {
        const char32_t a = lower(i + 1), b = lower(i + 2);
        if ((a == U'r' && b == U'e') || (a == U'v' && b == U'e') || (a == U'l' && b == U'l')) return i + 3;
        if (a == U's' || a == U't' || a == U'm' || a == U'd') return i + 2;
    }
    // Letters, with one mark before them.
    std::size_t j = i;
    if (!IsLetter(c[j]) && !IsNewline(c[j]) && !IsNumber(c[j]) && j + 1 < n && IsLetter(c[j + 1])) ++j;
    if (IsLetter(c[j])) {
        while (j < n && IsLetter(c[j])) ++j;
        return j;
    }
    if (IsNumber(c[i])) {
        j = i;
        while (j < n && j - i < 3 && IsNumber(c[j])) ++j;
        return j;
    }
    // Marks, with a space before them and line breaks after.
    j = i;
    if (c[j] == U' ' && j + 1 < n && !IsSpace(c[j + 1]) && !IsLetter(c[j + 1]) && !IsNumber(c[j + 1])) ++j;
    if (!IsSpace(c[j]) && !IsLetter(c[j]) && !IsNumber(c[j])) {
        while (j < n && !IsSpace(c[j]) && !IsLetter(c[j]) && !IsNumber(c[j])) ++j;
        while (j < n && IsNewline(c[j])) ++j;
        return j;
    }
    // White space: up to its last line break, or all but the last before
    // something else, or all of it.
    std::size_t end = i;
    while (end < n && IsSpace(c[end])) ++end;
    std::size_t lastBreak = 0;
    for (std::size_t k = i; k < end; ++k) {
        if (IsNewline(c[k])) lastBreak = k + 1;
    }
    if (lastBreak > i) return lastBreak;
    if (end == n) return end;
    if (end - i >= 2) return end - 1;
    return end;
}

}  // namespace

bool ByteBpe::Load(const std::filesystem::path& directory) {
    vocab_.clear();
    ids_.clear();
    ranks_.clear();
    cache_.clear();
    // Bytes as printable characters (GPT-2's byte-level map).
    std::vector<int> printable;
    for (int b = '!'; b <= '~'; ++b) printable.push_back(b);
    for (int b = 0xA1; b <= 0xAC; ++b) printable.push_back(b);
    for (int b = 0xAE; b <= 0xFF; ++b) printable.push_back(b);
    int extra = 0;
    for (int b = 0; b < 256; ++b) {
        const bool kept = std::find(printable.begin(), printable.end(), b) != printable.end();
        std::string ch;
        AppendUtf8(ch, kept ? static_cast<char32_t>(b) : static_cast<char32_t>(256 + extra++));
        byteChar_[b] = ch;
        charByte_[ch] = static_cast<std::uint8_t>(b);
    }
    std::ifstream vocab(directory / L"vocab.txt", std::ios::binary);
    for (std::string line; std::getline(vocab, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ids_.emplace(line, static_cast<std::int32_t>(vocab_.size()));
        vocab_.push_back(line);
    }
    std::ifstream merges(directory / L"merges.txt", std::ios::binary);
    std::int32_t rank = 0;
    for (std::string line; std::getline(merges, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto space = line.find(' ');
        if (space == std::string::npos) continue;
        ranks_.emplace(line.substr(0, space) + '\x01' + line.substr(space + 1), rank++);
    }
    if (const auto s = ids_.find("<|startoftext|>"); s != ids_.end()) start_ = s->second;
    if (const auto e = ids_.find("<|im_end|>"); e != ids_.end()) end_ = e->second;
    return !vocab_.empty() && !ranks_.empty();
}

void ByteBpe::EncodeWord(std::string_view word, std::vector<std::int32_t>& out) const {
    const std::string key(word);
    if (const auto hit = cache_.find(key); hit != cache_.end()) {
        out.insert(out.end(), hit->second.begin(), hit->second.end());
        return;
    }
    std::vector<std::string> parts;
    for (const char b : word) parts.push_back(byteChar_[static_cast<unsigned char>(b)]);
    while (parts.size() > 1) {
        std::int32_t best = std::numeric_limits<std::int32_t>::max();
        std::size_t at = 0;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            const auto r = ranks_.find(parts[k] + '\x01' + parts[k + 1]);
            if (r != ranks_.end() && r->second < best) {
                best = r->second;
                at = k;
            }
        }
        if (best == std::numeric_limits<std::int32_t>::max()) break;
        // Every occurrence of the pair, left to right.
        const std::string left = parts[at], right = parts[at + 1];
        std::vector<std::string> merged;
        for (std::size_t k = 0; k < parts.size(); ++k) {
            if (k + 1 < parts.size() && parts[k] == left && parts[k + 1] == right) {
                merged.push_back(left + right);
                ++k;
            } else {
                merged.push_back(parts[k]);
            }
        }
        parts = std::move(merged);
    }
    std::vector<std::int32_t> ids;
    for (const auto& p : parts) {
        if (const auto id = ids_.find(p); id != ids_.end()) ids.push_back(id->second);
    }
    out.insert(out.end(), ids.begin(), ids.end());
    if (cache_.size() < 65536) cache_.emplace(key, std::move(ids));
}

std::vector<std::int32_t> ByteBpe::Encode(std::string_view text, bool start) const {
    std::vector<std::int32_t> out;
    if (start) out.push_back(start_);
    const auto points = Utf8Points(text);
    for (std::size_t i = 0; i < points.cp.size();) {
        const std::size_t end = std::max(PieceEnd(points.cp, i), i + 1);
        EncodeWord(text.substr(points.at[i], points.at[end] - points.at[i]), out);
        i = end;
    }
    return out;
}

std::string ByteBpe::Decode(const std::vector<std::int32_t>& ids) const {
    std::string out;
    for (const auto id : ids) {
        if (id < 0 || static_cast<std::size_t>(id) >= vocab_.size()) continue;
        const auto& token = vocab_[static_cast<std::size_t>(id)];
        if (token.size() > 4 && token.starts_with("<|") && token.ends_with("|>")) continue;
        const auto points = Utf8Points(token);
        for (std::size_t k = 0; k < points.cp.size(); ++k) {
            std::string ch(token.substr(points.at[k], points.at[k + 1] - points.at[k]));
            if (const auto b = charByte_.find(ch); b != charByte_.end()) out += static_cast<char>(b->second);
        }
    }
    return out;
}

}  // namespace tekito::ai
