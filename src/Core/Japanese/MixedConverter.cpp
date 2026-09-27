#include "Core/Japanese/MixedConverter.h"

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KanaText.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace tekito::japanese {
namespace {

// Costs in the connection matrix's units (English words: see Costs).
constexpr std::int64_t kUnknownKanaCost = 10000;
constexpr std::int64_t kUnknownKeyCost = 20000;
constexpr std::int64_t kSpellingCorrectionPenalty = 5000;
constexpr std::size_t kShortestEnglishWord = 3;
constexpr std::size_t kLongestEnglishWord = 24;
constexpr std::int64_t kInfinity = std::numeric_limits<std::int64_t>::max() / 4;
constexpr std::size_t kNoKey = static_cast<std::size_t>(-1);

enum class NodeKind : std::uint8_t { Japanese, UnknownKana, English, UnknownKey };

struct Node {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint16_t left{0};
    std::uint16_t right{0};
    std::int64_t cost{0};
    NodeKind kind{NodeKind::Japanese};
    // Kana for Japanese nodes, the keys as typed otherwise.
    std::wstring text;
};

bool IsWordLetter(wchar_t ch) {
    return ch < 0x80 && std::iswalpha(ch);
}

}  // namespace

bool EnglishWords::Open(const std::filesystem::path& packDirectory) noexcept {
    lines_.clear();
    if (!file_.Open(packDirectory / L"words.tsv")) return false;
    const auto* data = file_.Data();
    const std::size_t size = file_.Size();
    try {
        std::size_t start = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (data[i] != '\n') continue;
            if (i > start) lines_.push_back(static_cast<std::uint32_t>(start));
            start = i + 1;
        }
    } catch (...) {
        lines_.clear();
    }
    return !lines_.empty();
}

std::optional<double> EnglishWords::Score(std::string_view word) const noexcept {
    if (lines_.empty() || word.empty()) return std::nullopt;
    const auto* data = reinterpret_cast<const char*>(file_.Data());
    const std::size_t size = file_.Size();
    const auto keyAt = [&](std::uint32_t offset) {
        std::size_t end = offset;
        while (end < size && data[end] != '\t' && data[end] != '\n') ++end;
        return std::string_view(data + offset, end - offset);
    };
    std::size_t lo = 0, hi = lines_.size();
    while (lo < hi) {
        const std::size_t middle = lo + (hi - lo) / 2;
        if (keyAt(lines_[middle]) < word) lo = middle + 1; else hi = middle;
    }
    if (lo >= lines_.size() || keyAt(lines_[lo]) != word) return std::nullopt;
    std::size_t at = lines_[lo] + word.size() + 1;
    double score = 0;
    double scale = 0;
    for (; at < size && data[at] != '\n' && data[at] != '\r'; ++at) {
        const char c = data[at];
        if (c == '.') {
            scale = 1;
        } else if (c >= '0' && c <= '9') {
            if (scale > 0) {
                scale /= 10;
                score += (c - '0') * scale;
            } else {
                score = score * 10 + (c - '0');
            }
        }
    }
    return score;
}

std::optional<MixedConversion> MixedConverter::Convert(std::wstring_view keys) const {
    const std::size_t n = keys.size();
    if (n < kShortestEnglishWord || !english_.IsOpen()) return std::nullopt;
    const std::uint16_t unknown = dictionary_.UnknownId();

    std::vector<Node> nodes;
    std::vector<std::vector<int>> beginningAt(n + 1), endingAt(n + 1);
    const auto add = [&](Node node) {
        const int index = static_cast<int>(nodes.size());
        beginningAt[node.begin].push_back(index);
        endingAt[node.end].push_back(index);
        nodes.push_back(std::move(node));
    };

    for (std::size_t i = 0; i < n; ++i) {
        // Japanese: the keys from here read as romaji, and the words those
        // kana start with (only where a word ends on a whole romaji unit).
        std::wstring kana;
        std::vector<std::size_t> keyAfter{i};  // key position after k kana, or kNoKey
        std::size_t position = i;
        for (const auto& token : JapaneseComposer::ParseRomaji(table_, keys.substr(i))) {
            if (token.leftover) break;
            position += token.keys;
            kana += token.kana;
            for (std::size_t c = 1; c < token.kana.size(); ++c) keyAfter.push_back(kNoKey);
            keyAfter.push_back(position);
        }
        if (!kana.empty()) {
            dictionary_.CommonPrefixSearch(dictionary_.Encode(kana), [&](std::size_t length, std::uint32_t record) {
                if (keyAfter[length] == kNoKey) return;
                dictionary_.ForEachWord(record, [&](const DictionaryWord& word) {
                    add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(keyAfter[length]), word.left,
                         word.right,
                         word.cost + (word.spellingCorrection ? kSpellingCorrectionPenalty : 0),
                         NodeKind::Japanese, kana.substr(0, length)});
                });
            });
            std::size_t first = 1;
            while (first < keyAfter.size() && keyAfter[first] == kNoKey) ++first;
            add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(keyAfter[first]), unknown, unknown,
                 kUnknownKanaCost, NodeKind::UnknownKana, kana.substr(0, first)});
        }

        // English: the keys from here as they are.
        std::string word;
        for (std::size_t j = i; j < n && j - i < kLongestEnglishWord && IsWordLetter(keys[j]); ++j) {
            word += static_cast<char>(std::towlower(keys[j]));
            if (word.size() < kShortestEnglishWord) continue;
            if (const auto score = english_.Score(word)) {
                const auto cost = costs_.englishBase -
                                  static_cast<std::int64_t>(static_cast<double>(costs_.englishPerScore) * *score) +
                                  costs_.languageSwitch;
                add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j + 1), unknown, unknown, cost,
                     NodeKind::English, std::wstring(keys.substr(i, j + 1 - i))});
            }
        }

        // A key neither language reads, as it is.
        add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1), unknown, unknown, kUnknownKeyCost,
             NodeKind::UnknownKey, std::wstring(1, keys[i])});
    }

    std::vector<std::int64_t> best(nodes.size(), kInfinity);
    std::vector<int> previous(nodes.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const int k : beginningAt[i]) {
            const Node& node = nodes[k];
            if (i == 0) {
                best[k] = node.cost + matrix_.Cost(0, node.left);
                continue;
            }
            for (const int p : endingAt[i]) {
                if (best[p] >= kInfinity) continue;
                const std::int64_t cost = best[p] + matrix_.Cost(nodes[p].right, node.left) + node.cost;
                if (cost < best[k]) {
                    best[k] = cost;
                    previous[k] = p;
                }
            }
        }
    }
    int last = -1;
    std::int64_t lastCost = kInfinity;
    for (const int p : endingAt[n]) {
        if (best[p] >= kInfinity) continue;
        const std::int64_t cost = best[p] + matrix_.Cost(nodes[p].right, 0);
        if (cost < lastCost) {
            lastCost = cost;
            last = p;
        }
    }
    std::vector<int> path;
    for (int k = last; k >= 0; k = previous[k]) path.push_back(k);
    std::reverse(path.begin(), path.end());

    const bool english = std::any_of(path.begin(), path.end(),
                                     [&](int k) { return nodes[k].kind == NodeKind::English; });
    const bool unreadable = std::any_of(path.begin(), path.end(),
                                        [&](int k) { return nodes[k].kind == NodeKind::UnknownKey; });
    if (path.empty() || !english || unreadable) return std::nullopt;

    // Japanese runs are converted as usual; each English word is a phrase.
    MixedConversion result;
    std::wstring run;
    const auto flushRun = [&]() {
        if (run.empty()) return;
        const std::size_t offset = result.reading.size();
        for (auto& phrase : converter_.Convert(run)) {
            phrase.begin += offset;
            result.phrases.push_back(std::move(phrase));
        }
        result.reading += run;
        run.clear();
    };
    for (const int k : path) {
        const Node& node = nodes[k];
        if (node.kind != NodeKind::English) {
            run += node.text;
            continue;
        }
        flushRun();
        Phrase phrase;
        phrase.begin = result.reading.size();
        phrase.length = node.text.size();
        phrase.candidates.push_back({node.text, 0, PhraseCandidate::Kind::English, false});
        // The same keys as romaji, in case Japanese was meant after all.
        std::wstring kana;
        bool readable = true;
        for (const auto& token : JapaneseComposer::ParseRomaji(table_, node.text)) {
            readable = readable && !token.leftover;
            kana += token.kana;
        }
        if (readable && !kana.empty()) {
            const auto katakana = ToKatakana(kana);
            for (const auto& text : {katakana, kana}) {
                phrase.candidates.push_back({text, 0, text == kana ? PhraseCandidate::Kind::Hiragana
                                                                   : PhraseCandidate::Kind::Katakana,
                                             false});
            }
        }
        result.reading += node.text;
        result.phrases.push_back(std::move(phrase));
    }
    flushRun();
    return result;
}

}  // namespace tekito::japanese
