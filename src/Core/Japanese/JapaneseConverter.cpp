#include "Core/Japanese/JapaneseConverter.h"

#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KanaText.h"

#include <algorithm>
#include <limits>

namespace tekito::japanese {
namespace {

// A character no word covers, and a run of characters the dictionary never
// uses (letters, full-width digits): costly, so words win when they can.
constexpr std::int64_t kUnknownCharacterCost = 10000;
constexpr std::int64_t kUnknownRunCost = 8000;
// Spelling-correction entries are offered, not chosen.
constexpr std::int64_t kSpellingCorrectionPenalty = 5000;
// Transliterations come after every dictionary candidate.
constexpr std::int64_t kTransliterationCost = std::numeric_limits<std::int32_t>::max();
constexpr std::size_t kBestPaths = 12;
constexpr std::int64_t kInfinity = std::numeric_limits<std::int64_t>::max() / 4;

struct Node {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint16_t left{0};
    std::uint16_t right{0};
    std::int64_t cost{0};
    bool known{false};
    DictionaryWord word;
};

struct Lattice {
    std::vector<Node> nodes;
    std::vector<std::vector<int>> beginningAt;
    std::vector<std::vector<int>> endingAt;
};

Lattice BuildLattice(const JapaneseDictionary& dictionary, const ReadingCodes& codes,
                     const std::vector<bool>& fixedBoundary) {
    const std::size_t n = codes.size();
    Lattice lattice;
    lattice.beginningAt.resize(n + 1);
    lattice.endingAt.resize(n + 1);
    const auto crosses = [&](std::size_t begin, std::size_t end) {
        for (std::size_t p = begin + 1; p < end; ++p) {
            if (fixedBoundary[p]) return true;
        }
        return false;
    };
    const auto add = [&](Node node) {
        const int index = static_cast<int>(lattice.nodes.size());
        lattice.beginningAt[node.begin].push_back(index);
        lattice.endingAt[node.end].push_back(index);
        lattice.nodes.push_back(node);
    };
    const std::uint16_t unknown = dictionary.UnknownId();
    for (std::size_t i = 0; i < n; ++i) {
        dictionary.CommonPrefixSearch(ReadingCodesView(codes).substr(i),
                                      [&](std::size_t length, std::uint32_t record) {
            if (crosses(i, i + length)) return;
            dictionary.ForEachWord(record, [&](const DictionaryWord& word) {
                Node node;
                node.begin = static_cast<std::uint32_t>(i);
                node.end = static_cast<std::uint32_t>(i + length);
                node.left = word.left;
                node.right = word.right;
                node.cost = word.cost + (word.spellingCorrection ? kSpellingCorrectionPenalty : 0);
                node.known = true;
                node.word = word;
                add(node);
            });
        });
        add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1), unknown, unknown,
             kUnknownCharacterCost, false, {}});
        if (codes[i] == 0 && (i == 0 || codes[i - 1] != 0)) {
            std::size_t end = i;
            while (end < n && codes[end] == 0 && (end == i || !fixedBoundary[end])) ++end;
            if (end - i > 1) {
                add({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(end), unknown, unknown,
                     kUnknownRunCost, false, {}});
            }
        }
    }
    return lattice;
}

struct PathEntry {
    std::int64_t cost{kInfinity};
    int previous{-1};
    int previousRank{-1};
};

}  // namespace

std::vector<Phrase> JapaneseConverter::Convert(std::wstring_view reading,
                                               std::span<const std::size_t> fixedLengths) const {
    const std::size_t n = reading.size();
    if (n == 0) return {};
    const ReadingCodes codes = dictionary_.Encode(reading);

    std::vector<bool> fixedBoundary(n + 1, false);
    std::size_t fixedEnd = 0;
    for (const std::size_t length : fixedLengths) {
        if (length == 0 || fixedEnd + length > n) break;
        fixedEnd += length;
        fixedBoundary[fixedEnd] = true;
    }
    const Lattice lattice = BuildLattice(dictionary_, codes, fixedBoundary);
    const auto& nodes = lattice.nodes;

    // The most likely words for the whole reading.
    std::vector<std::int64_t> best(nodes.size(), kInfinity);
    std::vector<int> previous(nodes.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const int k : lattice.beginningAt[i]) {
            const Node& node = nodes[k];
            if (i == 0) {
                best[k] = node.cost + matrix_.Cost(0, node.left);
                continue;
            }
            for (const int p : lattice.endingAt[i]) {
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
    for (const int p : lattice.endingAt[n]) {
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
    if (path.empty()) return {};

    // Phrases: fixed ones as given, then wherever the rules put a boundary.
    std::vector<std::pair<std::size_t, std::size_t>> spans;  // path index ranges
    std::size_t start = 0;
    for (std::size_t k = 1; k < path.size(); ++k) {
        const Node& before = nodes[path[k - 1]];
        const Node& node = nodes[path[k]];
        const bool split = fixedBoundary[node.begin] ||
                           (node.begin >= fixedEnd && matrix_.IsBoundary(before.right, node.left));
        if (split) {
            spans.emplace_back(start, k);
            start = k;
        }
    }
    spans.emplace_back(start, path.size());

    const auto textOf = [&](const Node& node) {
        const auto part = reading.substr(node.begin, node.end - node.begin);
        return node.known ? dictionary_.Surface(node.word, part) : std::wstring(part);
    };

    std::vector<Phrase> phrases;
    std::vector<std::vector<PathEntry>> entries(nodes.size());
    for (const auto& [first, end] : spans) {
        const std::uint32_t begin = nodes[path[first]].begin;
        const std::uint32_t finish = nodes[path[end - 1]].end;
        const std::uint16_t leftContext = first == 0 ? 0 : nodes[path[first - 1]].right;
        const std::uint16_t rightContext = end == path.size() ? 0 : nodes[path[end]].left;

        // The likeliest ways to write this phrase between its neighbors.
        for (std::uint32_t i = begin; i < finish; ++i) {
            for (const int k : lattice.beginningAt[i]) {
                const Node& node = nodes[k];
                auto& list = entries[k];
                list.clear();
                if (node.end > finish) continue;
                if (i == begin) {
                    list.push_back({node.cost + matrix_.Cost(leftContext, node.left), -1, -1});
                } else {
                    for (const int p : lattice.endingAt[i]) {
                        if (nodes[p].begin < begin) continue;
                        const int connection = matrix_.Cost(nodes[p].right, node.left);
                        for (std::size_t r = 0; r < entries[p].size(); ++r) {
                            list.push_back({entries[p][r].cost + connection + node.cost, p,
                                            static_cast<int>(r)});
                        }
                    }
                }
                if (list.size() > kBestPaths) {
                    std::partial_sort(list.begin(), list.begin() + kBestPaths, list.end(),
                                      [](const PathEntry& a, const PathEntry& b) { return a.cost < b.cost; });
                    list.resize(kBestPaths);
                } else {
                    std::sort(list.begin(), list.end(),
                              [](const PathEntry& a, const PathEntry& b) { return a.cost < b.cost; });
                }
            }
        }
        struct Ending {
            std::int64_t cost;
            int node;
            int rank;
        };
        std::vector<Ending> endings;
        for (const int k : lattice.endingAt[finish]) {
            if (nodes[k].begin < begin) continue;
            const int connection = matrix_.Cost(nodes[k].right, rightContext);
            for (std::size_t r = 0; r < entries[k].size(); ++r) {
                endings.push_back({entries[k][r].cost + connection, k, static_cast<int>(r)});
            }
        }
        std::sort(endings.begin(), endings.end(),
                  [](const Ending& a, const Ending& b) { return a.cost < b.cost; });

        Phrase phrase;
        phrase.begin = begin;
        phrase.length = finish - begin;
        const auto has = [&](const std::wstring& text) {
            return std::any_of(phrase.candidates.begin(), phrase.candidates.end(),
                               [&](const PhraseCandidate& c) { return c.text == text; });
        };
        for (const auto& ending : endings) {
            if (phrase.candidates.size() >= kBestPaths) break;
            std::wstring text;
            bool spelling = false;
            std::vector<int> parts;
            for (int k = ending.node, r = ending.rank; k >= 0;) {
                parts.push_back(k);
                const PathEntry& entry = entries[k][static_cast<std::size_t>(r)];
                k = entry.previous;
                r = entry.previousRank;
            }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                text += textOf(nodes[*it]);
                spelling = spelling || (nodes[*it].known && nodes[*it].word.spellingCorrection);
            }
            if (!has(text)) {
                phrase.candidates.push_back({std::move(text), ending.cost,
                                             PhraseCandidate::Kind::Dictionary, spelling});
            }
        }

        // Every word read exactly like the phrase (homophones), by likelihood.
        const auto phraseReading = reading.substr(begin, finish - begin);
        if (const auto record = dictionary_.Find(ReadingCodesView(codes).substr(begin, finish - begin))) {
            std::vector<PhraseCandidate> words;
            dictionary_.ForEachWord(*record, [&](const DictionaryWord& word) {
                const std::int64_t cost = matrix_.Cost(leftContext, word.left) + word.cost +
                                          (word.spellingCorrection ? kSpellingCorrectionPenalty : 0) +
                                          matrix_.Cost(word.right, rightContext);
                words.push_back({dictionary_.Surface(word, phraseReading), cost,
                                 PhraseCandidate::Kind::Dictionary, word.spellingCorrection});
            });
            std::stable_sort(words.begin(), words.end(),
                             [](const PhraseCandidate& a, const PhraseCandidate& b) { return a.cost < b.cost; });
            for (auto& word : words) {
                if (phrase.candidates.size() >= kMaxCandidates) break;
                if (!has(word.text)) phrase.candidates.push_back(std::move(word));
            }
        }

        const std::wstring hiragana(phraseReading);
        const std::wstring katakana = ToKatakana(phraseReading);
        if (!has(hiragana)) {
            phrase.candidates.push_back({hiragana, kTransliterationCost, PhraseCandidate::Kind::Hiragana, false});
        }
        if (!has(katakana)) {
            phrase.candidates.push_back({katakana, kTransliterationCost, PhraseCandidate::Kind::Katakana, false});
        }
        phrases.push_back(std::move(phrase));
    }
    return phrases;
}

std::vector<Prediction> JapaneseConverter::Predict(std::wstring_view reading, std::size_t limit) const {
    // Going through more keys than this would take too long per keystroke.
    constexpr std::uint32_t kMaxKeys = 3000;
    std::vector<Prediction> predictions;
    if (reading.empty() || limit == 0) return predictions;
    const ReadingCodes codes = dictionary_.Encode(reading);
    const auto [first, end] = dictionary_.PrefixRange(codes);
    if (first >= end || end - first > kMaxKeys) return predictions;

    struct Found {
        std::int64_t cost;
        std::uint32_t record;
        DictionaryWord word;
    };
    std::vector<Found> found;
    for (std::uint32_t record = first; record < end; ++record) {
        bool taken = false;
        dictionary_.ForEachWord(record, [&](const DictionaryWord& word) {
            // The cheapest word of each longer reading, never a spelling
            // correction.
            if (taken || word.spellingCorrection) return;
            taken = true;
            found.push_back({word.cost, record, word});
        });
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.cost < b.cost; });
    for (const auto& item : found) {
        std::wstring key = dictionary_.KeyText(item.record);
        if (key.size() <= reading.size()) continue;
        std::wstring text = dictionary_.Surface(item.word, key);
        const bool seen = std::any_of(predictions.begin(), predictions.end(),
                                      [&](const Prediction& p) { return p.text == text; });
        if (seen) continue;
        predictions.push_back({std::move(key), std::move(text), item.cost});
        if (predictions.size() >= limit) break;
    }
    return predictions;
}

}  // namespace tekito::japanese
