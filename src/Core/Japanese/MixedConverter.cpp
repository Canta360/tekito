#include "Core/Japanese/MixedConverter.h"

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KanaText.h"
#include "Core/TypoModel.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <set>
#include <tuple>

namespace tekito::japanese {
namespace {

// Costs in the connection matrix's units (English words: see Costs).
constexpr std::int64_t kUnknownKanaCost = 10000;
constexpr std::int64_t kUnknownKeyCost = 20000;
constexpr std::int64_t kSpellingCorrectionPenalty = 5000;
constexpr std::size_t kShortestEnglishWord = 3;
constexpr std::size_t kLongestEnglishWord = 24;
// Slips are looked for this many keys into a word, and a corrected word is
// at most this long; longer words are rare enough to leave as typed.
constexpr std::size_t kTypoWindow = 12;
// Keys a slip may have left out: a vowel, the n of ん.
constexpr std::wstring_view kDroppedKeys = L"aiueon";
// Of the words a corrected reading can be, the cheapest this many.
constexpr std::size_t kCorrectedForms = 2;
// Corrected words allowed per this many keys (at least one): more slips than
// that is not Japanese mistyped but something else (English the words pack
// lacks, a name), left as typed.
constexpr std::size_t kKeysPerSlip = 12;
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
    // A Japanese word read through a slip in the keys.
    bool corrected{false};
};

// One slip in a run of keys, undone: `keys` is the run as meant, and a
// position in it maps back to one in the run as typed.
struct Slip {
    enum class Kind : std::uint8_t { Replaced, Dropped, Added, Swapped } kind;
    std::size_t at;
    std::wstring keys;

    // Keys as typed consumed by the first `meant` keys as meant.
    [[nodiscard]] std::size_t Typed(std::size_t meant) const noexcept {
        switch (kind) {
            case Kind::Dropped: return meant <= at ? meant : meant - 1;
            case Kind::Added: return meant <= at ? meant : meant + 1;
            default: return meant;
        }
    }
    // Whether a word over the first `meant` keys needs the slip undone.
    [[nodiscard]] bool Covered(std::size_t meant) const noexcept {
        return kind == Kind::Swapped ? meant > at + 1 : meant > at;
    }
};

// The runs one slip away from `typed`, with the slip at `at` (at the end:
// a key left out last).
void SlipsAt(std::wstring_view typed, std::size_t at, std::vector<Slip>& slips) {
    const wchar_t key = at < typed.size() ? typed[at] : L'\0';
    const bool letter = key >= L'a' && key <= L'z';
    if (letter) {
        for (wchar_t other = L'a'; other <= L'z'; ++other) {
            if (!AreQwertyNeighbors(key, other)) continue;
            std::wstring keys(typed);
            keys[at] = other;
            slips.push_back({Slip::Kind::Replaced, at, std::move(keys)});
        }
        // An extra key typed.
        std::wstring keys(typed);
        keys.erase(at, 1);
        slips.push_back({Slip::Kind::Added, at, std::move(keys)});
    }
    // A key left out before this one: a vowel, n, or the doubled consonant
    // of small tsu.
    std::wstring dropped(kDroppedKeys);
    if (letter && kDroppedKeys.find(key) == std::wstring_view::npos) dropped += key;
    for (const wchar_t missing : dropped) {
        std::wstring keys(typed);
        keys.insert(at, 1, missing);
        slips.push_back({Slip::Kind::Dropped, at, std::move(keys)});
    }
    if (letter && at + 1 < typed.size() && typed[at + 1] != key && typed[at + 1] >= L'a' && typed[at + 1] <= L'z') {
        std::wstring keys(typed);
        std::swap(keys[at], keys[at + 1]);
        slips.push_back({Slip::Kind::Swapped, at, std::move(keys)});
    }
}

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
    if (n < kShortestEnglishWord) return std::nullopt;
    const std::uint16_t unknown = dictionary_.UnknownId();

    std::vector<Node> nodes;
    std::vector<std::vector<int>> beginningAt(n + 1), endingAt(n + 1);
    const auto add = [&](Node node) {
        const int index = static_cast<int>(nodes.size());
        beginningAt[node.begin].push_back(index);
        endingAt[node.end].push_back(index);
        nodes.push_back(std::move(node));
    };

    // Japanese words from key `i`: those `kana` starts with, where a word
    // ends on a whole romaji unit (keyAfter[k] is the key after the first k
    // kana, kNoKey inside a unit). Corrected words must be at least
    // `shortest` kana long (to take in the slip); each is added once, in its
    // likeliest forms only.
    std::set<std::tuple<std::size_t, std::size_t, std::wstring>> corrections;
    const auto addWords = [&](std::size_t i, const std::wstring& kana, const std::vector<std::size_t>& keyAfter,
                              bool corrected, std::size_t shortest) {
        dictionary_.CommonPrefixSearch(dictionary_.Encode(kana), [&](std::size_t length, std::uint32_t record) {
            const std::size_t end = keyAfter[length];
            if (end == kNoKey || length < shortest) return;
            if (corrected && !corrections.insert({i, end, kana.substr(0, length)}).second) return;
            std::size_t forms = 0;
            dictionary_.ForEachWord(record, [&](const DictionaryWord& word) {
                add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(end), word.left, word.right,
                     word.cost + (word.spellingCorrection ? kSpellingCorrectionPenalty : 0) +
                         (corrected ? costs_.typo : 0),
                     NodeKind::Japanese, kana.substr(0, length), corrected});
                return !corrected || ++forms < kCorrectedForms;
            });
        });
    };
    // The keys from `i` read as romaji, up to the first unit the table
    // cannot read.
    const auto readWords = [&](std::size_t i) {
        std::wstring kana;
        std::vector<std::size_t> keyAfter{i};
        std::size_t position = i;
        for (const auto& token : JapaneseComposer::ParseRomaji(table_, keys.substr(i))) {
            if (token.leftover) break;
            position += token.keys;
            kana += token.kana;
            for (std::size_t c = 1; c < token.kana.size(); ++c) keyAfter.push_back(kNoKey);
            keyAfter.push_back(position);
        }
        if (kana.empty()) return;
        addWords(i, kana, keyAfter, false, 1);
        std::size_t first = 1;
        while (first < keyAfter.size() && keyAfter[first] == kNoKey) ++first;
        add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(keyAfter[first]), unknown, unknown,
             kUnknownKanaCost, NodeKind::UnknownKana, kana.substr(0, first)});
    };

    for (std::size_t i = 0; i < n; ++i) {
        readWords(i);

        // English: the keys from here as they are.
        std::string word;
        for (std::size_t j = i; j < n && english_.IsOpen() && j - i < kLongestEnglishWord && IsWordLetter(keys[j]);
             ++j) {
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

    std::vector<std::int64_t> best;
    std::vector<int> previous;
    const auto bestPath = [&]() {
        best.assign(nodes.size(), kInfinity);
        previous.assign(nodes.size(), -1);
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
        return path;
    };
    std::vector<int> path = bestPath();

    // Words one slip away from the keys, the slip within the word. A slip
    // only changes the romaji from the unit before it (that unit may be an
    // n the next key decides), so the units before that are read once, from
    // the start.
    if (costs_.typo > 0 && !path.empty()) {
        const auto units = JapaneseComposer::ParseRomaji(table_, keys);
        std::vector<std::size_t> bounds{0};
        for (const auto& unit : units) bounds.push_back(bounds.back() + unit.keys);

        std::vector<Slip> slips;
        const std::size_t before = nodes.size();
        for (std::size_t g = 0; g <= n && !units.empty(); ++g) {
            // The unit the slip is in, and the one before it.
            std::size_t unit = static_cast<std::size_t>(std::upper_bound(bounds.begin(), bounds.end(), g) -
                                                        bounds.begin()) - 1;
            unit = std::min(unit, units.size() - 1);
            const std::size_t from = unit > 0 ? unit - 1 : 0;
            const std::size_t fromKey = bounds[from];
            const std::size_t toKey = std::min(n, g + kTypoWindow);
            slips.clear();
            SlipsAt(keys.substr(fromKey, toKey - fromKey), g - fromKey, slips);
            for (const auto& slip : slips) {
                // The keys as meant from `fromKey` (and from the slip's own
                // unit, when that starts a word), then earlier units in front.
                for (std::size_t start = from; start <= unit; ++start) {
                    const std::size_t skip = bounds[start] - fromKey;
                    if (skip > slip.at) break;
                    std::wstring kana;
                    std::vector<std::size_t> keyAfter{bounds[start]};
                    std::size_t meant = skip;
                    std::size_t shortest = kNoKey;
                    for (const auto& token : JapaneseComposer::ParseRomaji(table_, std::wstring_view(slip.keys).substr(skip))) {
                        if (token.leftover) break;
                        meant += token.keys;
                        kana += token.kana;
                        for (std::size_t c = 1; c < token.kana.size(); ++c) keyAfter.push_back(kNoKey);
                        keyAfter.push_back(fromKey + slip.Typed(meant));
                        if (shortest == kNoKey && slip.Covered(meant)) shortest = kana.size();
                    }
                    if (shortest == kNoKey) continue;
                    addWords(bounds[start], kana, keyAfter, true, shortest);
                    if (start != from) continue;
                    // Earlier starts: the units before, as typed.
                    for (std::size_t earlier = from; earlier-- > 0;) {
                        if (units[earlier].leftover || g - bounds[earlier] >= kTypoWindow) break;
                        const std::wstring& unitKana = units[earlier].kana;
                        kana.insert(0, unitKana);
                        std::vector<std::size_t> front{bounds[earlier]};
                        for (std::size_t c = 1; c < unitKana.size(); ++c) front.push_back(kNoKey);
                        keyAfter.insert(keyAfter.begin(), front.begin(), front.end());
                        shortest += unitKana.size();
                        addWords(bounds[earlier], kana, keyAfter, true, shortest);
                    }
                }
            }
        }
        if (nodes.size() > before) path = bestPath();
    }

    const bool english = std::any_of(path.begin(), path.end(),
                                     [&](int k) { return nodes[k].kind == NodeKind::English; });
    const auto slips = static_cast<std::size_t>(
        std::count_if(path.begin(), path.end(), [&](int k) { return nodes[k].corrected; }));
    if (slips > std::max<std::size_t>(1, n / kKeysPerSlip)) return std::nullopt;
    const bool corrected = slips > 0;
    const bool unreadable = std::any_of(path.begin(), path.end(),
                                        [&](int k) { return nodes[k].kind == NodeKind::UnknownKey; });
    if (path.empty() || !(english || corrected) || unreadable) return std::nullopt;

    // Japanese runs are converted as usual; each English word is a phrase.
    MixedConversion result;
    result.english = english;
    result.corrected = corrected;
    // The reading is the path's texts in order.
    std::size_t readingSize = 0;
    for (const int k : path) readingSize += nodes[k].text.size();
    result.keyAt.assign(readingSize + 1, std::wstring::npos);
    for (std::size_t offset = 0; const int k : path) {
        result.keyAt[offset] = nodes[k].begin;
        offset += nodes[k].text.size();
        result.keyAt[offset] = nodes[k].end;
    }
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
