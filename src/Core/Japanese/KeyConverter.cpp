#include "Core/Japanese/KeyConverter.h"

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KanaText.h"
#include "Core/TypoModel.h"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace tekito::japanese {
namespace {

// Costs in the connection matrix's units (slips: see Costs).
constexpr std::int64_t kUnknownKanaCost = 10000;
constexpr std::int64_t kUnknownKeyCost = 20000;
constexpr std::int64_t kSpellingCorrectionPenalty = 5000;
// Fewer keys than this are left as typed.
constexpr std::size_t kShortestKeys = 3;
// Slips are looked for this many keys into a word, and a corrected word is
// at most this long; longer words are rare enough to leave as typed.
constexpr std::size_t kTypoWindow = 12;
// Keys a slip may have left out: a vowel, the n of ん.
constexpr std::wstring_view kDroppedKeys = L"aiueon";
// With Costs::consonantsDropped, the consonants too.
constexpr std::wstring_view kDroppedConsonants = L"kgsztdhbpmyrwjfc";
// Of the words a corrected reading can be, the cheapest this many.
constexpr std::size_t kCorrectedForms = 2;
// Corrected words allowed per this many keys (at least one): more slips than
// that is not Japanese mistyped but something else (English, a name), left
// as typed.
constexpr std::size_t kKeysPerSlip = 12;
constexpr std::int64_t kInfinity = std::numeric_limits<std::int64_t>::max() / 4;
constexpr std::size_t kNoKey = static_cast<std::size_t>(-1);

enum class NodeKind : std::uint8_t { Japanese, UnknownKana, UnknownKey };

struct Node {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint16_t left{0};
    std::uint16_t right{0};
    std::int64_t cost{0};
    NodeKind kind{NodeKind::Japanese};
    // Kana, or the key as typed for an unknown key.
    std::wstring text;
    // A Japanese word read through a slip in the keys, and what the slip
    // costs.
    bool corrected{false};
    std::int64_t slip{0};
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
void SlipsAt(std::wstring_view typed, std::size_t at, bool consonants, bool vowels, std::vector<Slip>& slips) {
    const wchar_t key = at < typed.size() ? typed[at] : L'\0';
    const bool letter = key >= L'a' && key <= L'z';
    constexpr std::wstring_view kVowels = L"aiueo";
    const bool vowel = vowels && kVowels.find(key) != std::wstring_view::npos;
    if (letter) {
        for (wchar_t other = L'a'; other <= L'z'; ++other) {
            const bool vowelFor = vowel && other != key && kVowels.find(other) != std::wstring_view::npos;
            if (!AreQwertyNeighbors(key, other) && !vowelFor) continue;
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
    if (consonants) dropped += kDroppedConsonants;
    if (letter && dropped.find(key) == std::wstring::npos) dropped += key;
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

}  // namespace

std::optional<KeyConversion> KeyConverter::Convert(std::wstring_view keys, std::uint16_t context,
                                                   std::span<const std::wstring> contextWords,
                                                   const JapaneseUserDictionary* user) const {
    const std::size_t n = keys.size();
    if (n < kShortestKeys || costs_.typo <= 0) return std::nullopt;
    // A slip in a word starting at key `at`: dearer early, cheaper late.
    const auto SlipCost = [&](std::size_t at) {
        const std::int64_t percent =
            costs_.earlyPercent +
            (costs_.latePercent - costs_.earlyPercent) * static_cast<std::int64_t>(at) / static_cast<std::int64_t>(n);
        return costs_.typo * percent / 100;
    };
    const std::uint16_t unknown = dictionary_.UnknownId();
    const auto KindPercent = [&](Slip::Kind kind) {
        switch (kind) {
            case Slip::Kind::Replaced: return costs_.replacedPercent;
            case Slip::Kind::Dropped: return costs_.droppedPercent;
            case Slip::Kind::Added: return costs_.addedPercent;
            case Slip::Kind::Swapped: return costs_.swappedPercent;
        }
        return std::int64_t{100};
    };
    // The first key typed twice, in keys long enough to be a phrase.
    const bool doubledFirst = n >= costs_.doubledFirstKeys && keys[0] == keys[1] &&
                              std::wstring_view(L"aiueon").find(keys[0]) == std::wstring_view::npos;

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
                              bool corrected, std::size_t shortest, std::int64_t slipCost = 0) {
        dictionary_.CommonPrefixSearch(dictionary_.Encode(kana), [&](std::size_t length, std::uint32_t record) {
            const std::size_t end = keyAfter[length];
            if (end == kNoKey || length < shortest) return;
            if (corrected && !corrections.insert({i, end, kana.substr(0, length)}).second) return;
            std::size_t forms = 0;
            dictionary_.ForEachWord(record, [&](const DictionaryWord& word) {
                add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(end), word.left, word.right,
                     word.cost + (word.spellingCorrection ? kSpellingCorrectionPenalty : 0) + slipCost,
                     NodeKind::Japanese, kana.substr(0, length), corrected, slipCost});
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
        // A key the table does not read, as it is.
        add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1), unknown, unknown, kUnknownKeyCost,
             NodeKind::UnknownKey, std::wstring(1, keys[i])});
    }

    // The likeliest path, and the likeliest with no slip and with one or
    // more: each node's best cost on paths that have (1) or have not (0)
    // undone a slip up to it.
    std::vector<std::int64_t> best[2];
    std::vector<int> previous[2];
    std::vector<int> paths[2];
    const auto bestPath = [&]() {
        for (int s = 0; s < 2; ++s) {
            best[s].assign(nodes.size(), kInfinity);
            previous[s].assign(nodes.size(), -1);
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (const int k : beginningAt[i]) {
                const Node& node = nodes[k];
                const int s = node.corrected ? 1 : 0;
                if (i == 0) {
                    best[s][k] = node.cost + matrix_.Cost(context, node.left);
                    continue;
                }
                for (const int p : endingAt[i]) {
                    for (int from = 0; from < 2; ++from) {
                        if (best[from][p] >= kInfinity) continue;
                        const int to = from | s;
                        const std::int64_t cost = best[from][p] + matrix_.Cost(nodes[p].right, node.left) + node.cost;
                        if (cost < best[to][k]) {
                            best[to][k] = cost;
                            previous[to][k] = p * 2 + from;
                        }
                    }
                }
            }
        }
        std::int64_t pathCost[2] = {kInfinity, kInfinity};
        for (int s = 0; s < 2; ++s) {
            int last = -1;
            for (const int p : endingAt[n]) {
                if (best[s][p] >= kInfinity) continue;
                const std::int64_t cost = best[s][p] + matrix_.Cost(nodes[p].right, 0);
                if (cost < pathCost[s]) {
                    pathCost[s] = cost;
                    last = p;
                }
            }
            paths[s].clear();
            for (int k = last, layer = s; k >= 0;) {
                paths[s].push_back(k);
                const int back = previous[layer][k];
                k = back < 0 ? -1 : back / 2;
                layer = back < 0 ? 0 : back % 2;
            }
            std::reverse(paths[s].begin(), paths[s].end());
        }
        return pathCost[1] < pathCost[0] ? paths[1] : paths[0];
    };
    std::vector<int> path = bestPath();

    // Words one slip away from the keys, the slip within the word. A slip
    // only changes the romaji from the unit before it (that unit may be an
    // n the next key decides), so the units before that are read once, from
    // the start.
    if (!path.empty()) {
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
            SlipsAt(keys.substr(fromKey, toKey - fromKey), g - fromKey, costs_.consonantsDropped, costs_.vowelsReplaced, slips);
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
                    const bool firstTwice = doubledFirst && slip.kind == Slip::Kind::Added && fromKey + slip.at <= 1;
                    // An n left out stays dear: nn before a vowel reads two
                    // ways (kakuninnonegai: 確認お願い, not 確認の願い).
                    const bool droppedN = slip.kind == Slip::Kind::Dropped && slip.keys[slip.at] == L'n';
                    const std::int64_t kindPercent = droppedN ? 100 : KindPercent(slip.kind);
                    const std::int64_t slipCost = firstTwice ? costs_.typo * costs_.doubledFirstPercent / 100
                                                             : SlipCost(bounds[start]) * kindPercent / 100;
                    addWords(bounds[start], kana, keyAfter, true, shortest, slipCost);
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
                        addWords(bounds[earlier], kana, keyAfter, true, shortest,
                                 SlipCost(bounds[earlier]) * kindPercent / 100);
                    }
                }
            }
        }
        if (nodes.size() > before) path = bestPath();
    }

    // The keys as typed against the likeliest slip undone, converted with
    // the language model after the text before them.
    if (costs_.contextDecides && !paths[1].empty()) {
        const auto readingOf = [&](const std::vector<int>& p) {
            std::wstring reading;
            for (const int k : p) reading += nodes[k].text;
            return reading;
        };
        const auto readable = [&](const std::vector<int>& p) {
            return !p.empty() && std::none_of(p.begin(), p.end(), [&](int k) { return nodes[k].kind == NodeKind::UnknownKey; });
        };
        std::int64_t slipCosts = 0;
        for (const int k : paths[1]) slipCosts += nodes[k].slip;
        const auto typed = readable(paths[0]) ? converter_.Cost(readingOf(paths[0]), context, user) : std::nullopt;
        const auto meant = readable(paths[1]) ? converter_.Cost(readingOf(paths[1]), context, user) : std::nullopt;
        if (meant && (!typed || *meant + slipCosts + costs_.keepTypedBy < *typed)) {
            path = paths[1];
        } else if (typed) {
            return std::nullopt;
        }
    }

    const auto slips = static_cast<std::size_t>(
        std::count_if(path.begin(), path.end(), [&](int k) { return nodes[k].corrected; }));
    if (slips > std::max<std::size_t>(1, n / kKeysPerSlip)) return std::nullopt;
    const bool unreadable = std::any_of(path.begin(), path.end(),
                                        [&](int k) { return nodes[k].kind == NodeKind::UnknownKey; });
    if (path.empty() || slips == 0 || unreadable) return std::nullopt;

    // The corrected reading is converted as usual.
    KeyConversion result;
    for (const int k : path) result.reading += nodes[k].text;
    result.keyAt.assign(result.reading.size() + 1, std::wstring::npos);
    for (std::size_t offset = 0; const int k : path) {
        result.keyAt[offset] = nodes[k].begin;
        offset += nodes[k].text.size();
        result.keyAt[offset] = nodes[k].end;
    }
    result.phrases = converter_.Convert(result.reading, {}, context, contextWords, user);
    return result;
}

}  // namespace tekito::japanese
