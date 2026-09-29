#include "Core/Japanese/JapaneseConverter.h"

#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/LanguageModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

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
// How far the language model moves a pair, in nats.
constexpr double kPmiFloor = -5.0;
constexpr double kPmiCeiling = 8.0;
// The sentence model reorders only this many of a phrase's candidates.
constexpr std::size_t kTopicCandidates = 8;

struct Node {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint16_t left{0};
    std::uint16_t right{0};
    std::int64_t cost{0};
    bool known{false};
    DictionaryWord word;
    // A word the user added, and how it is written.
    bool user{false};
    std::wstring text;
    // For the language model, when it has the word: ln(count), the pair
    // hash the word starts, and its text to end a pair.
    std::optional<double> logCount;
    std::uint64_t pairStart{0};
    std::wstring surface;
};

struct Lattice {
    std::vector<Node> nodes;
    std::vector<std::vector<int>> beginningAt;
    std::vector<std::vector<int>> endingAt;
};

bool IsDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'\xFF10' && c <= L'\xFF19');
}

// A decimal point or thousands comma, as typed in Japanese.
bool IsNumberMark(wchar_t c) {
    return c == L'.' || c == L',' || c == L'\xFF0E' || c == L'\xFF0C';
}

Lattice BuildLattice(const JapaneseDictionary& dictionary, const ReadingCodes& codes,
                     const std::vector<bool>& fixedBoundary, std::wstring_view reading,
                     const LanguageModel* model, const JapaneseUserDictionary* user,
                     const JapaneseConverter::NumberWord* number) {
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
        lattice.nodes.push_back(std::move(node));
    };
    const std::uint16_t unknown = dictionary.UnknownId();
    for (std::size_t i = 0; i < n; ++i) {
        dictionary.CommonPrefixSearch(ReadingCodesView(codes).substr(i),
                                      [&](std::size_t length, std::uint32_t record) {
            if (crosses(i, i + length)) return;
            dictionary.ForEachWord(record, [&](const DictionaryWord& word) {
                if (user && user->HasSuppressed()) {
                    const auto part = reading.substr(i, length);
                    if (user->Suppresses(part, dictionary.Surface(word, part))) return;
                }
                Node node;
                node.begin = static_cast<std::uint32_t>(i);
                node.end = static_cast<std::uint32_t>(i + length);
                node.left = word.left;
                node.right = word.right;
                node.cost = word.cost + (word.spellingCorrection ? kSpellingCorrectionPenalty : 0);
                node.known = true;
                node.word = word;
                if (model) {
                    auto surface = dictionary.Surface(word, reading.substr(i, length));
                    node.logCount = model->LogCount(surface);
                    if (node.logCount) {
                        node.pairStart = LanguageModel::PairStart(surface);
                        node.surface = std::move(surface);
                    }
                }
                add(std::move(node));
            });
        });
        if (user) {
            user->ForEachPrefixOf(reading.substr(i), [&](std::size_t length, const JapaneseUserDictionary::Entry& entry) {
                if (crosses(i, i + length)) return;
                Node node;
                node.begin = static_cast<std::uint32_t>(i);
                node.end = static_cast<std::uint32_t>(i + length);
                node.left = entry.left;
                node.right = entry.right;
                node.cost = entry.cost;
                node.user = true;
                node.text = entry.surface;
                if (model) {
                    node.logCount = model->LogCount(entry.surface);
                    if (node.logCount) {
                        node.pairStart = LanguageModel::PairStart(entry.surface);
                        node.surface = entry.surface;
                    }
                }
                add(std::move(node));
            });
        }
        // A number: the digits from here, with the marks between them.
        if (number && IsDigit(reading[i]) && (i == 0 || !IsDigit(reading[i - 1]))) {
            std::size_t end = i;
            while (end < n && (end == i || !fixedBoundary[end]) &&
                   (IsDigit(reading[end]) || (IsNumberMark(reading[end]) && end + 1 < n && IsDigit(reading[end + 1])))) {
                ++end;
            }
            Node digits;
            digits.begin = static_cast<std::uint32_t>(i);
            digits.end = static_cast<std::uint32_t>(end);
            digits.left = number->left;
            digits.right = number->right;
            digits.cost = number->cost;
            add(std::move(digits));
        }
        Node single;
        single.begin = static_cast<std::uint32_t>(i);
        single.end = static_cast<std::uint32_t>(i + 1);
        single.left = single.right = unknown;
        single.cost = kUnknownCharacterCost;
        add(std::move(single));
        if (codes[i] == 0 && (i == 0 || codes[i - 1] != 0)) {
            std::size_t end = i;
            while (end < n && codes[end] == 0 && (end == i || !fixedBoundary[end])) ++end;
            if (end - i > 1) {
                Node run;
                run.begin = static_cast<std::uint32_t>(i);
                run.end = static_cast<std::uint32_t>(end);
                run.left = run.right = unknown;
                run.cost = kUnknownRunCost;
                add(std::move(run));
            }
        }
    }
    return lattice;
}

// What the language model takes off for `right` following `left`.
// A pair never seen is as if seen half a time: negative for words common
// enough that they would have met; no evidence either way otherwise.
std::int64_t PairBonus(const LanguageModel* model, const JapaneseConverter::ModelWeights& weights, const Node& left,
                       const Node& right) {
    if (!model || !left.logCount || !right.logCount) return 0;
    double pmi = 0.0;
    if (const auto seen = model->PairPmi(left.pairStart, right.surface)) {
        pmi = *seen >= weights.threshold ? *seen - weights.threshold : std::min(0.0, *seen);
    } else {
        pmi = std::min(0.0, std::log(0.5) + model->LogTokens() - *left.logCount - *right.logCount);
    }
    pmi = std::clamp(pmi, kPmiFloor, kPmiCeiling);
    return std::llround(pmi * static_cast<double>(pmi >= 0 ? weights.costPerNat : weights.penaltyPerNat));
}

struct PathEntry {
    std::int64_t cost{kInfinity};
    int previous{-1};
    int previousRank{-1};
};

}  // namespace

std::vector<Phrase> JapaneseConverter::Convert(std::wstring_view reading,
                                               std::span<const std::size_t> fixedLengths,
                                               std::uint16_t context,
                                               std::span<const std::wstring> contextWords,
                                               const JapaneseUserDictionary* user,
                                               std::span<const std::size_t> breaks) const {
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
    for (const std::size_t at : breaks) {
        if (at > 0 && at < n) fixedBoundary[at] = true;
    }
    const bool pairs = model_ && (weights_.costPerNat > 0 || weights_.penaltyPerNat > 0);
    const Lattice lattice = BuildLattice(dictionary_, codes, fixedBoundary, reading, pairs ? model_ : nullptr, user,
                                         number_ ? &*number_ : nullptr);
    const auto& nodes = lattice.nodes;

    // The most likely words for the whole reading.
    std::vector<std::int64_t> best(nodes.size(), kInfinity);
    std::vector<int> previous(nodes.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const int k : lattice.beginningAt[i]) {
            const Node& node = nodes[k];
            if (i == 0) {
                best[k] = node.cost + matrix_.Cost(context, node.left);
                continue;
            }
            for (const int p : lattice.endingAt[i]) {
                if (best[p] >= kInfinity) continue;
                const std::int64_t cost = best[p] + matrix_.Cost(nodes[p].right, node.left) + node.cost -
                                          PairBonus(model_, weights_, nodes[p], node);
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
        return node.user ? node.text : node.known ? dictionary_.Surface(node.word, part) : std::wstring(part);
    };
    // The content words of the likeliest reading, by phrase, for the
    // sentence model: each phrase's candidates meet the others' words.
    const bool topics = model_ && weights_.costPerTopic > 0;
    std::vector<std::pair<std::size_t, std::wstring>> sentenceWords;  // (span index, word)
    if (topics) {
        // Words written just before belong to no phrase here.
        for (const auto& word : contextWords) sentenceWords.emplace_back(spans.size(), word);
        for (std::size_t s = 0; s < spans.size(); ++s) {
            for (std::size_t k = spans[s].first; k < spans[s].second; ++k) {
                if (!nodes[path[k]].known && !nodes[path[k]].user) continue;
                auto word = textOf(nodes[path[k]]);
                if (LanguageModel::IsContentWord(word)) sentenceWords.emplace_back(s, std::move(word));
            }
        }
    }

    std::vector<Phrase> phrases;
    std::vector<std::vector<PathEntry>> entries(nodes.size());
    for (std::size_t spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
        const auto [first, end] = spans[spanIndex];
        const std::uint32_t begin = nodes[path[first]].begin;
        const std::uint32_t finish = nodes[path[end - 1]].end;
        const std::uint16_t leftContext = first == 0 ? context : nodes[path[first - 1]].right;
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
                        const std::int64_t connection =
                            matrix_.Cost(nodes[p].right, node.left) - PairBonus(model_, weights_, nodes[p], node);
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
        // The content word each candidate stands on (its longest), for the
        // sentence model.
        std::vector<std::wstring> heads;
        for (const auto& ending : endings) {
            if (phrase.candidates.size() >= kBestPaths) break;
            std::wstring text;
            std::wstring head;
            bool spelling = false;
            std::vector<int> parts;
            for (int k = ending.node, r = ending.rank; k >= 0;) {
                parts.push_back(k);
                const PathEntry& entry = entries[k][static_cast<std::size_t>(r)];
                k = entry.previous;
                r = entry.previousRank;
            }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                auto part = textOf(nodes[*it]);
                if (topics && (nodes[*it].known || nodes[*it].user) && part.size() > head.size() &&
                    LanguageModel::IsContentWord(part)) {
                    head = part;
                }
                text += part;
                spelling = spelling || (nodes[*it].known && nodes[*it].word.spellingCorrection);
            }
            if (!has(text)) {
                PhraseCandidate candidate{std::move(text), ending.cost, PhraseCandidate::Kind::Dictionary, spelling};
                candidate.rightId = nodes[ending.node].right;
                phrase.candidates.push_back(std::move(candidate));
                heads.push_back(std::move(head));
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
                PhraseCandidate candidate{dictionary_.Surface(word, phraseReading), cost,
                                          PhraseCandidate::Kind::Dictionary, word.spellingCorrection};
                candidate.rightId = word.right;
                words.push_back(std::move(candidate));
            });
            std::stable_sort(words.begin(), words.end(),
                             [](const PhraseCandidate& a, const PhraseCandidate& b) { return a.cost < b.cost; });
            for (auto& word : words) {
                if (phrase.candidates.size() >= kMaxCandidates) break;
                if (!has(word.text)) {
                    heads.push_back(topics && LanguageModel::IsContentWord(word.text) ? word.text : std::wstring{});
                    phrase.candidates.push_back(std::move(word));
                }
            }
        }
        if (topics && !heads.empty() && !heads.front().empty()) {
            // When the first choice stands on a content word, the candidates
            // that do too are put in order again with each one's strongest
            // tie to the other phrases' words taken off its cost; the rest
            // keep their places (kana and particles are not told apart so).
            std::vector<std::size_t> places;
            std::vector<std::pair<std::int64_t, PhraseCandidate>> ranked;
            for (std::size_t c = 0; c < phrase.candidates.size() && c < heads.size() && c < kTopicCandidates; ++c) {
                if (heads[c].empty()) continue;
                double strongest = 0.0;
                for (const auto& [where, word] : sentenceWords) {
                    if (where == spanIndex) continue;
                    if (const auto tie = model_->Topic(heads[c], word)) strongest = std::max(strongest, *tie);
                }
                places.push_back(c);
                ranked.emplace_back(phrase.candidates[c].cost - std::llround(strongest * static_cast<double>(weights_.costPerTopic)),
                                    phrase.candidates[c]);
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t r = 0; r < ranked.size(); ++r) phrase.candidates[places[r]] = std::move(ranked[r].second);
        }

        // The user's words read like the phrase come first.
        if (const auto* words = user ? user->Exact(phraseReading) : nullptr) {
            std::vector<PhraseCandidate> userFirst;
            for (const auto& word : *words) {
                PhraseCandidate candidate{word.surface,
                                          matrix_.Cost(leftContext, word.left) + word.cost +
                                              matrix_.Cost(word.right, rightContext),
                                          PhraseCandidate::Kind::Dictionary, false};
                candidate.rightId = word.right;
                userFirst.push_back(std::move(candidate));
            }
            std::stable_sort(userFirst.begin(), userFirst.end(),
                             [](const PhraseCandidate& a, const PhraseCandidate& b) { return a.cost < b.cost; });
            std::erase_if(phrase.candidates, [&](const PhraseCandidate& c) {
                return std::any_of(userFirst.begin(), userFirst.end(), [&](const PhraseCandidate& f) { return f.text == c.text; });
            });
            phrase.candidates.insert(phrase.candidates.begin(), std::make_move_iterator(userFirst.begin()),
                                     std::make_move_iterator(userFirst.end()));
        }

        // The words only to offer come second (moved there if the
        // dictionary has them further down), after the first choice.
        if (const auto* words = user ? user->Suggested(phraseReading) : nullptr) {
            std::size_t at = std::min<std::size_t>(1, phrase.candidates.size());
            for (const auto& word : *words) {
                const auto found = std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                                [&](const PhraseCandidate& c) { return c.text == word.surface; });
                const auto index = static_cast<std::size_t>(found - phrase.candidates.begin());
                if (found != phrase.candidates.end() && index < at) continue;  // already first
                PhraseCandidate candidate{word.surface,
                                          matrix_.Cost(leftContext, word.left) + word.cost +
                                              matrix_.Cost(word.right, rightContext),
                                          PhraseCandidate::Kind::Dictionary, false};
                candidate.rightId = word.right;
                if (found != phrase.candidates.end()) phrase.candidates.erase(found);
                phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(at++),
                                         std::move(candidate));
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
        // What the user never wants for this reading goes; the kana stay if
        // nothing else would.
        if (user && user->HasSuppressed()) {
            std::erase_if(phrase.candidates,
                          [&](const PhraseCandidate& c) { return user->Suppresses(phraseReading, c.text); });
            if (phrase.candidates.empty()) {
                phrase.candidates.push_back({hiragana, kTransliterationCost, PhraseCandidate::Kind::Hiragana, false});
            }
        }
        phrases.push_back(std::move(phrase));
    }
    return phrases;
}

std::optional<PhraseCandidate> JapaneseConverter::Best(std::wstring_view reading, std::uint16_t context,
                                                      const JapaneseUserDictionary* user) const {
    const std::size_t n = reading.size();
    if (n == 0) return std::nullopt;
    const ReadingCodes codes = dictionary_.Encode(reading);
    const std::vector<bool> noBoundary(n + 1, false);
    // Quick on purpose: the language model is left out.
    const Lattice lattice = BuildLattice(dictionary_, codes, noBoundary, reading, nullptr, user,
                                         number_ ? &*number_ : nullptr);
    const auto& nodes = lattice.nodes;
    std::vector<std::int64_t> best(nodes.size(), kInfinity);
    std::vector<int> previous(nodes.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const int k : lattice.beginningAt[i]) {
            const Node& node = nodes[k];
            if (!node.known && !node.user) continue;
            if (i == 0) {
                best[k] = node.cost + matrix_.Cost(context, node.left);
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
    if (last < 0) return std::nullopt;
    std::vector<int> path;
    for (int k = last; k >= 0; k = previous[k]) path.push_back(k);
    PhraseCandidate result{{}, lastCost, PhraseCandidate::Kind::Dictionary, false};
    result.rightId = nodes[last].right;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const Node& node = nodes[*it];
        result.text += node.user ? node.text
                                 : dictionary_.Surface(node.word, reading.substr(node.begin, node.end - node.begin));
        result.spellingCorrection = result.spellingCorrection || node.word.spellingCorrection;
    }
    // A text the user never wants for this reading is not the likeliest.
    if (user && user->Suppresses(reading, result.text)) return std::nullopt;
    return result;
}

std::vector<Prediction> JapaneseConverter::Predict(std::wstring_view reading, std::size_t limit,
                                                  const JapaneseUserDictionary* user) const {
    // Going through more keys than this would take too long per keystroke.
    constexpr std::uint32_t kMaxKeys = 2000;
    std::vector<Prediction> predictions;
    if (reading.empty() || limit == 0) return predictions;
    if (user) {
        for (const auto* entry : user->StartingWith(reading)) {
            if (predictions.size() >= limit) break;
            predictions.push_back({entry->reading, entry->surface, entry->cost});
        }
    }
    const ReadingCodes codes = dictionary_.Encode(reading);
    auto [first, end] = dictionary_.PrefixRange(codes);
    // Too many to look through: the dictionary's words are left out.
    if (end - first > kMaxKeys) end = first;

    struct Found {
        std::int64_t cost;
        std::uint32_t record;
        DictionaryWord word;
    };
    std::vector<Found> found;
    found.reserve(end - first);
    for (std::uint32_t record = first; record < end; ++record) {
        dictionary_.ForEachWord(record, [&](const DictionaryWord& word) {
            // The cheapest word of each longer reading, never a spelling
            // correction.
            if (word.spellingCorrection) return true;
            found.push_back({word.cost, record, word});
            return false;
        });
    }
    const auto cheaper = [](const Found& a, const Found& b) { return a.cost < b.cost; };
    // Only the first few are needed; some may be dropped below.
    const std::size_t sorted = std::min(found.size(), limit * 4);
    std::partial_sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(sorted), found.end(), cheaper);
    found.resize(sorted);
    for (const auto& item : found) {
        std::wstring key = dictionary_.KeyText(item.record);
        if (key.size() <= reading.size()) continue;
        std::wstring text = dictionary_.Surface(item.word, key);
        if (user && user->Suppresses(key, text)) continue;
        const bool seen = std::any_of(predictions.begin(), predictions.end(),
                                      [&](const Prediction& p) { return p.text == text; });
        if (seen) continue;
        if (predictions.size() >= limit) break;
        predictions.push_back({std::move(key), std::move(text), item.cost});
    }
    // The words only to offer come second.
    if (user) {
        std::size_t at = std::min<std::size_t>(1, predictions.size());
        for (const auto* entry : user->StartingWith(reading, true)) {
            const auto existing = std::find_if(predictions.begin(), predictions.end(),
                                               [&](const Prediction& p) { return p.text == entry->surface; });
            if (existing != predictions.end() && static_cast<std::size_t>(existing - predictions.begin()) < at) continue;
            if (existing != predictions.end()) predictions.erase(existing);
            predictions.insert(predictions.begin() + static_cast<std::ptrdiff_t>(at++),
                               {entry->reading, entry->surface, entry->cost});
        }
        if (predictions.size() > limit) predictions.resize(limit);
    }
    return predictions;
}

}  // namespace tekito::japanese
