// Offline accuracy of Japanese conversion: converts each reading of
// eval/generated/japanese_eval.tsv (scripts/ja/prepare-japanese-eval.py) with
// the japanese-core pack and compares the first conversion with the
// acceptable ones.
//
// tekito_ja_eval --pack <data/ja/japanese-core> --corpus <japanese_eval.tsv> [--show-misses N]
// tekito_ja_eval --pack <data/ja/japanese-core> --keys-corpus <japanese_eval_keys.tsv>
//                --romaji <data/ja/japanese-romaji> [--typo N] [--dump-phrases <path>]
//                [--write-sentences <path>]
// tekito_ja_eval --pack <data/ja/japanese-core> --guess-readings <sentences.txt> <out.tsv>
//                [--by-phrase [--no-context]]
//                [--lm <data/ja/japanese-lm> [--lm-scale N] [--lm-penalty N] [--lm-threshold X] [--topic-scale N]]
// tekito_ja_eval --pack <data/ja/japanese-core> --keys-corpus <keys.tsv> --romaji <data/ja/japanese-romaji>
//                --study hints|live|register [--join N] [--sample N] [--show-misses N] [--lm ...]
//
// The second form types the romaji keys into the composer, as TEKITO does,
// and converts with Space. With --by-phrase each sentence is typed phrase
// by phrase instead (the phrases its whole conversion makes), converting
// and committing each, so what was committed leads into the next; with
// --no-context each phrase starts afresh.
//
// --study measures ways of typing borrowed from SKK (JapaneseTypingStudy.h).
//
// --wikimedia <data/ja/japanese-wikimedia> adds the words of Wikipedia
// article titles to the dictionary, as TEKITO does when the pack is there.
//
// Per source: sentences whose first conversion is acceptable, the character
// error rate against the closest acceptable text, how often an acceptable
// text is among the first phrase-by-phrase choices, and conversion time.

#include "Core/ConversionEngine.h"
#include "Core/EnglishSpelling.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/LetterOdds.h"
#include "Core/Japanese/EdgeScorer.h"
#include "Core/Japanese/Notation.h"
#include "UserData/JapaneseSettings.h"
#include "Core/Japanese/WrittenForms.h"
#include "Core/SpecialConversions.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/Japanese/AiJudge.h"
#include "Core/Japanese/ChunkJudge.h"
#include "Core/Japanese/Compounds.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/KanjiSets.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/TopicVectors.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/Japanese/Collocations.h"
#include "Core/Japanese/EnglishTies.h"
#include "Core/Japanese/Homophones.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/Japanese/KanaSlips.h"
#include "JapaneseEvalCommon.h"
#include "JapaneseTypingStudy.h"

#include <cwctype>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <limits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// TEKITO_EVAL_NOTATION_LEVEL: how words are written, 1-5 (3, the default).
int NotationLevel() {
    const char* level = std::getenv("TEKITO_EVAL_NOTATION_LEVEL");
    return level && *level ? std::clamp(static_cast<int>(std::strtol(level, nullptr, 10)), 1, 5) : 3;
}

// Digits and numbers read in kana as one number word, as the text service
// sets it up (JapaneseData: japanese-core/pos.tsv, "number").
void SetNumberWord(tekito::japanese::JapaneseConverter& converter, const std::filesystem::path& pack) {
    const auto parts = tekito::japanese::UserPartsOfSpeech::Load(pack / "pos.tsv");
    if (const auto number = parts.Number()) {
        converter.SetNumberWord(tekito::japanese::JapaneseConverter::NumberWord{number->left, number->right, number->cost});
    }
    // A particle first at the start of a sentence costs more, as JapaneseData
    // sets it up (1000); TEKITO_EVAL_JOINED_START=<cost> another (0: off).
    // Each row is taken as starting a sentence.
    if (const auto noun = parts.For(tekito::japanese::UserWordKind::Noun)) {
        const char* cost = std::getenv("TEKITO_EVAL_JOINED_START");
        converter.SetJoinedStart(noun->right, cost && *cost ? std::strtoll(cost, nullptr, 10) : 1000);
    }
    // Names the sentence check leaves, as JapaneseData sets them up
    // (TEKITO_EVAL_NAME_PARTS=0: none).
    if (const char* off = std::getenv("TEKITO_EVAL_NAME_PARTS"); !off || *off != '0') {
        std::vector<std::uint16_t> names;
        for (const auto kind : {tekito::japanese::UserWordKind::Organization, tekito::japanese::UserWordKind::ProperNoun}) {
            if (const auto part = parts.For(kind)) names.push_back(part->left);
        }
        const auto organization = parts.For(tekito::japanese::UserWordKind::Organization);
        converter.SetNameParts(std::move(names), organization ? organization->left : 0);
    }
    // Letters read by their names (ぴーしー: PC), as JapaneseData sets them up
    // (1500 under a proper noun); TEKITO_EVAL_LETTER_COST: another cost, or
    // "off".
    if (const auto noun = parts.For(tekito::japanese::UserWordKind::ProperNoun)) {
        const char* cost = std::getenv("TEKITO_EVAL_LETTER_COST");
        if (!cost || std::string_view(cost) != "off") {
            const std::int32_t over = cost && *cost ? static_cast<std::int32_t>(std::strtol(cost, nullptr, 10)) : -1500;
            converter.SetLetterWord(tekito::japanese::JapaneseConverter::NumberWord{noun->left, noun->right, noun->cost + over});
            // Known abbreviations, as JapaneseData has them (written-forms and
            // english-names beside the English data).
            static tekito::japanese::WrittenForms forms, names;
            static tekito::japanese::Loanwords latin;
            if (!latin.HasJapaneseTextCounts()) (void)latin.OpenEnglish(tekito::ExternalLexiconProvider::PackDirectory(L"japanese-english"));
            if (!forms.IsOpen()) (void)forms.Open(tekito::ExternalLexiconProvider::PackDirectory(L"written-forms"));
            if (!names.IsOpen()) (void)names.Open(tekito::ExternalLexiconProvider::PackDirectory(L"english-names"));
            converter.SetKnownLetters([](std::wstring_view letters) {
                std::wstring lower(letters);
                for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
                if (const auto seen = latin.InJapaneseText(lower); seen && *seen >= 1.6) return true;
                for (const auto* table : {&forms, &names}) {
                    if (!table->IsOpen() || (table == &names && letters.size() < 3)) continue;
                    if (const auto form = table->Find(lower); form && form->written == letters) return true;
                }
                return false;
            });
        }
    }
    // TEKITO_EVAL_HEAD_WORDS=0: a phrase's candidates without the words read
    // like its first word (紅藻が for こうそうが).
    if (const char* heads = std::getenv("TEKITO_EVAL_HEAD_WORDS"); heads && *heads == '0') converter.SetHeadWords(false);
    // TEKITO_EVAL_NOTATION=<directory>: how words are written by level
    // (japanese-notation, as JapaneseData loads it); TEKITO_EVAL_NOTATION_COST:
    // what the unwanted form costs more (3000); the level is NotationLevel's.
    static tekito::japanese::Notation notation;
    if (const char* directory = std::getenv("TEKITO_EVAL_NOTATION"); directory && *directory) {
        if (notation.IsOpen() || notation.Open(directory)) {
            const char* cost = std::getenv("TEKITO_EVAL_NOTATION_COST");
            converter.SetNotation(&notation, cost && *cost ? std::strtoll(cost, nullptr, 10) : 3000);
        }
    }
    // TEKITO_EVAL_EDGES=<directory>: the edge scorer (edges.bin, chars.txt)
    // takes its scores off the words' costs; TEKITO_EVAL_EDGE_MARGIN: for
    // the words within this of the cheapest path (5000);
    // TEKITO_EVAL_EDGE_WEIGHT: the scores times this (1).
    static tekito::japanese::EdgeScorer edges;
    if (const char* directory = std::getenv("TEKITO_EVAL_EDGES"); directory && *directory) {
        if (edges.IsOpen() || edges.Open(directory)) {
            const char* margin = std::getenv("TEKITO_EVAL_EDGE_MARGIN");
            const char* weight = std::getenv("TEKITO_EVAL_EDGE_WEIGHT");
            converter.SetEdgeScorer(&edges, margin && *margin ? std::strtoll(margin, nullptr, 10) : 5000,
                                    weight && *weight ? std::strtof(weight, nullptr) : 1.0f);
        } else {
            std::cerr << "could not open the edge scorer at " << directory << "\n";
        }
    }
    // TEKITO_EVAL_COMPOUNDS=<pack directory>: compounds put together from
    // kanji that join readily, at TEKITO_EVAL_COMPOUND_COST per nat (200).
    static tekito::japanese::Compounds compounds;
    if (const char* directory = std::getenv("TEKITO_EVAL_COMPOUNDS");
        directory && *directory && (compounds.IsOpen() || compounds.Open(directory))) {
        const char* cost = std::getenv("TEKITO_EVAL_COMPOUND_COST");
        converter.SetCompounds(&compounds, cost && *cost ? std::strtoll(cost, nullptr, 10) : 200);
    }
}

using tekito::japanese::ConnectionMatrix;
using tekito::japanese::JapaneseConverter;
using tekito::japanese::JapaneseDictionary;
using tekito::japanese::Phrase;
using Clock = std::chrono::steady_clock;
using ja_eval::EditDistance;
using ja_eval::LoadCorpus;
using ja_eval::Narrow;
using ja_eval::Percentile;
using ja_eval::Row;
using ja_eval::Widen;

// Whether an acceptable text can be put together from each phrase's first
// `within` candidates (the same split as the first conversion).
bool ReachableWithin(const std::vector<Phrase>& phrases, const std::vector<std::wstring>& expected,
                     std::size_t within) {
    for (const auto& target : expected) {
        std::vector<std::size_t> offsets{0};
        for (const auto& phrase : phrases) {
            std::vector<std::size_t> next;
            for (const std::size_t offset : offsets) {
                for (std::size_t i = 0; i < phrase.candidates.size() && i < within; ++i) {
                    const auto& text = phrase.candidates[i].text;
                    if (target.compare(offset, text.size(), text) == 0) next.push_back(offset + text.size());
                }
            }
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            offsets = std::move(next);
            if (offsets.empty()) break;
        }
        if (std::find(offsets.begin(), offsets.end(), target.size()) != offsets.end()) return true;
    }
    return false;
}

// The `count` likeliest whole sentences from each phrase's candidate list
// (likeliest first), taking the sum of the ranks picked as how unlikely a
// sentence is: the first conversion, then those one pick away, and so on.
std::vector<std::wstring> SentencesByRank(const std::vector<std::vector<std::wstring>>& lists, std::size_t count) {
    std::vector<std::wstring> sentences;
    if (lists.empty() || std::any_of(lists.begin(), lists.end(), [](const auto& l) { return l.empty(); })) {
        return sentences;
    }
    using Picks = std::vector<std::size_t>;
    const auto rankSum = [](const Picks& picks) {
        std::size_t sum = 0;
        for (const std::size_t p : picks) sum += p;
        return sum;
    };
    const auto later = [&](const Picks& a, const Picks& b) { return rankSum(a) > rankSum(b) || (rankSum(a) == rankSum(b) && a > b); };
    std::priority_queue<Picks, std::vector<Picks>, decltype(later)> queue(later);
    std::set<Picks> seen;
    queue.push(Picks(lists.size(), 0));
    seen.insert(queue.top());
    std::set<std::wstring> written;
    while (!queue.empty() && sentences.size() < count) {
        const Picks picks = queue.top();
        queue.pop();
        std::wstring text;
        for (std::size_t i = 0; i < lists.size(); ++i) text += lists[i][picks[i]];
        if (written.insert(text).second) sentences.push_back(std::move(text));
        for (std::size_t i = 0; i < lists.size(); ++i) {
            if (picks[i] + 1 >= lists[i].size()) continue;
            Picks next = picks;
            ++next[i];
            if (seen.insert(next).second) queue.push(std::move(next));
        }
    }
    return sentences;
}

// The same, but by how much likelier the converter found each pick than the
// phrase's first (its cost less the first's), so a second choice almost as
// likely as the first comes before one far behind. `deltas` go with
// `lists`, each list's rising (the place in the list kept).
std::vector<std::wstring> SentencesByCost(const std::vector<std::vector<std::wstring>>& lists,
                                          const std::vector<std::vector<std::int64_t>>& deltas, std::size_t count) {
    std::vector<std::wstring> sentences;
    if (lists.empty() || std::any_of(lists.begin(), lists.end(), [](const auto& l) { return l.empty(); })) {
        return sentences;
    }
    using Picks = std::vector<std::size_t>;
    const auto costOf = [&](const Picks& picks) {
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < picks.size(); ++i) sum += deltas[i][picks[i]];
        return sum;
    };
    using Entry = std::pair<std::int64_t, Picks>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    std::set<Picks> seen;
    queue.push({0, Picks(lists.size(), 0)});
    seen.insert(Picks(lists.size(), 0));
    std::set<std::wstring> written;
    while (!queue.empty() && sentences.size() < count) {
        const Picks picks = queue.top().second;
        queue.pop();
        std::wstring text;
        for (std::size_t i = 0; i < lists.size(); ++i) text += lists[i][picks[i]];
        if (written.insert(text).second) sentences.push_back(std::move(text));
        for (std::size_t i = 0; i < lists.size(); ++i) {
            if (picks[i] + 1 >= lists[i].size()) continue;
            Picks next = picks;
            ++next[i];
            if (seen.insert(next).second) queue.push({costOf(next), std::move(next)});
        }
    }
    return sentences;
}

// TEKITO_EVAL_SENTENCE_ORDER=cost puts the whole sentences together by
// SentencesByCost instead of by rank; TEKITO_EVAL_KANA_COST (default 3000)
// is what a kana or other candidate without a cost of its own is taken to
// cost over the phrase's first.
bool SentencesByCostChosen() {
    static const bool chosen = [] {
        const char* order = std::getenv("TEKITO_EVAL_SENTENCE_ORDER");
        return order && std::string_view(order) == "cost";
    }();
    return chosen;
}
std::int64_t KanaCost() {
    static const std::int64_t cost = [] {
        const char* value = std::getenv("TEKITO_EVAL_KANA_COST");
        return value && *value ? std::stoll(value) : std::int64_t{3000};
    }();
    return cost;
}

// TEKITO_EVAL_OUTPUT=<path>: each row's text as committed (source, id, text),
// in the layout the real-input benchmark writes, to be scored the same way.
void WriteOutput(const std::string& source, const std::string& id, const std::wstring& text) {
    static std::ofstream out = [] {
        const char* path = std::getenv("TEKITO_EVAL_OUTPUT");
        return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
    }();
    if (out.is_open()) out << source << "\t" << id << "\t" << Narrow(text) << "\n";
}

// TEKITO_EVAL_SHOWN=<path>: what the composition showed just before Enter
// (live conversion, Mix holding a sentence), in the same layout.
void WriteShown(const std::string& source, const std::string& id, const tekito::japanese::JapaneseComposer& composer) {
    std::wstring text;
    for (const auto& segment : composer.Segments()) text += segment.text;
    static std::ofstream out = [] {
        const char* path = std::getenv("TEKITO_EVAL_SHOWN");
        return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
    }();
    if (out.is_open()) out << source << "\t" << id << "\t" << Narrow(text) << "\n";
}

// TEKITO_EVAL_PRODUCT_OFF=special,digits,numbers,letters,predict,correction: those of
// TEKITO_EVAL_PRODUCT's settings back as the evaluation had them.
void UndoProductSettings(tekito::japanese::JapaneseComposer& composer) {
    const char* off = std::getenv("TEKITO_EVAL_PRODUCT_OFF");
    if (!off || !*off) return;
    const std::string_view list(off);
    const auto has = [&](std::string_view name) { return list.find(name) != std::string_view::npos; };
    if (has("special")) composer.SetSpecialConversions(nullptr);
    if (has("digits")) composer.SetHalfWidthDigits(false);
    if (has("numbers")) composer.SetNumberStyle(tekito::japanese::JapaneseComposer::NumberStyle::AsTyped);
    if (has("letters")) composer.SetHalfWidthLetters(false);
    if (has("predict")) composer.SetPredictionEnabled(false);
    if (has("correction")) composer.SetEnglishCorrection(false);
}

struct Totals {
    std::size_t rows{0};
    std::size_t exact{0};
    std::size_t top10{0};
    std::size_t errors{0};
    std::size_t characters{0};
    std::vector<double> microseconds;
};

// --ai light|strong: the AI model asked after the sentence check
// (TekitoAI.exe next to this program, the model installed).
const tekito::japanese::AiJudge* gAiJudge = nullptr;
// --ai probe: no model; the sentences an AI model would read (its Top() of
// the sentence check's, then the whole texts) are kept for each row, to see
// how often what was meant is among them. Nothing is answered.
class ProbeJudge final : public tekito::japanese::AiJudge {
public:
    // TEKITO_EVAL_PROBE_TOP=N: how many of the sentence check's (10).
    [[nodiscard]] std::size_t Top() const noexcept override {
        static const std::size_t top = [] {
            const char* value = std::getenv("TEKITO_EVAL_PROBE_TOP");
            return value && *value ? std::strtoul(value, nullptr, 10) : 10ul;
        }();
        return top;
    }
    [[nodiscard]] double Margin() const noexcept override { return 1e9; }
    [[nodiscard]] std::optional<tekito::japanese::AiConversion> Convert(std::wstring_view,
                                                                       std::span<const std::wstring> sentences,
                                                                       std::wstring_view, bool) const override {
        asked.emplace_back(sentences.begin(), sentences.end());
        return std::nullopt;
    }
    mutable std::vector<std::vector<std::wstring>> asked;
};
ProbeJudge gProbe;

// TEKITO_EVAL_ASKED=<path>: with --ai probe, those sentences in the layout
// --write-sentences writes (source, id, rank, meant 1/0, text).
void WriteAsked(const std::string& source, const std::string& id, const std::vector<std::wstring>& asked,
                std::size_t meant) {
    static std::ofstream out = [] {
        const char* path = std::getenv("TEKITO_EVAL_ASKED");
        return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
    }();
    if (!out.is_open()) return;
    for (std::size_t k = 0; k < asked.size(); ++k) {
        out << source << "\t" << id << "\t" << k << "\t" << (k == meant ? 1 : 0) << "\t" << Narrow(asked[k]) << "\n";
    }
}
// TEKITO_EVAL_CHECKED=<path>: with --ai probe, for a row converted at once,
// the sentences the sentence check chose among (source, id, k, whole 1/0,
// meant 1/0, chosen 1/0, text as shown, text as read).
void WriteChecked(const std::string& source, const std::string& id,
                  const tekito::japanese::JapaneseComposer::CheckedSentences& checked,
                  const std::vector<std::wstring>& expected) {
    static std::ofstream out = [] {
        const char* path = std::getenv("TEKITO_EVAL_CHECKED");
        return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
    }();
    if (!out.is_open()) return;
    for (std::size_t k = 0; k < checked.shown.size(); ++k) {
        const bool meant = std::find(expected.begin(), expected.end(), checked.shown[k]) != expected.end();
        out << source << "\t" << id << "\t" << k << "\t" << (k >= checked.ranked ? 1 : 0) << "\t" << (meant ? 1 : 0) << "\t"
            << (k == checked.chosen ? 1 : 0) << "\t" << Narrow(checked.shown[k]) << "\t" << Narrow(checked.read[k]) << "\n";
    }
}
// TEKITO_EVAL_FEATURES=<path>: for each row's last sentence check, the
// sentences it chose among with what it knew of each (source, id, k,
// meant 1/0, chosen 1/0, the SentenceFeature values, text as shown).
void WriteFeatures(const std::string& source, const std::string& id,
                   const tekito::japanese::JapaneseComposer::CheckedSentences& checked,
                   const std::vector<std::wstring>& expected) {
    static std::ofstream out = [] {
        const char* path = std::getenv("TEKITO_EVAL_FEATURES");
        return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
    }();
    static std::uint64_t written = 0;
    if (!out.is_open() || checked.serial == written || checked.features.size() != checked.shown.size()) return;
    written = checked.serial;
    for (std::size_t k = 0; k < checked.shown.size(); ++k) {
        const bool meant = std::find(expected.begin(), expected.end(), checked.shown[k]) != expected.end();
        out << source << "\t" << id << "\t" << k << "\t" << (meant ? 1 : 0) << "\t" << (k == checked.chosen ? 1 : 0);
        for (const float f : checked.features[k]) out << "\t" << f;
        out << "\t" << Narrow(checked.shown[k]) << "\n";
    }
}
// Why what was meant is not among the sentences the AI reads, best first:
// its phrases are those the check read and each piece among the candidates
// it read (only the combination was not reached), among all the phrase's
// candidates (left out of the check's), or one the dictionary writes for
// the phrase's reading but not a candidate; or the reading has to break
// elsewhere; or the dictionary does not write it at all.
enum class Miss { Combination, Listed, Writable, Breaks, Dictionary };
const char* const kMissNames[] = {"combination", "listed", "writable", "breaks", "dictionary"};

Miss WhyMissed(const tekito::japanese::JapaneseConverter& converter, std::wstring_view reading,
               const std::vector<tekito::japanese::JapaneseComposer::CheckedPhrase>& phrases, std::wstring_view meant) {
    if (!converter.CanWrite(reading, meant)) return Miss::Dictionary;
    // best[i][j]: the best level (3 read, 2 listed, 1 writable) at which
    // the first i phrases write meant's first j characters.
    const std::size_t m = meant.size();
    std::vector<std::vector<int>> best(phrases.size() + 1, std::vector<int>(m + 1, 0));
    best[0][0] = 3;
    for (std::size_t i = 0; i < phrases.size(); ++i) {
        const auto& phrase = phrases[i];
        const std::size_t longest = phrase.reading.size() * 2 + 2;
        for (std::size_t j = 0; j < m; ++j) {
            if (best[i][j] == 0) continue;
            for (std::size_t k = j + 1; k <= m && k - j <= longest; ++k) {
                const auto piece = std::wstring(meant.substr(j, k - j));
                int level = 0;
                if (std::find(phrase.read.begin(), phrase.read.end(), piece) != phrase.read.end()) level = 3;
                else if (std::find(phrase.all.begin(), phrase.all.end(), piece) != phrase.all.end()) level = 2;
                else if (converter.CanWrite(phrase.reading, piece)) level = 1;
                best[i + 1][k] = std::max(best[i + 1][k], std::min(best[i][j], level));
            }
        }
    }
    switch (best[phrases.size()][m]) {
    case 3: return Miss::Combination;
    case 2: return Miss::Listed;
    case 1: return Miss::Writable;
    default: return Miss::Breaks;
    }
}

// TEKITO_EVAL_MISSES=<path>: with --ai probe, each row whose meaning is not
// among the AI's sentences (source, id, why, reading, meant, written).
void WriteMiss(const std::string& source, const std::string& id, Miss why, const std::wstring& reading,
               const std::wstring& meant, const std::wstring& output,
               const std::vector<tekito::japanese::JapaneseComposer::CheckedPhrase>& phrases) {
    static std::ofstream out = [] {
        const char* path = std::getenv("TEKITO_EVAL_MISSES");
        return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
    }();
    if (out.is_open()) {
        out << source << "\t" << id << "\t" << kMissNames[static_cast<int>(why)] << "\t" << Narrow(reading) << "\t"
            << Narrow(meant) << "\t" << Narrow(output) << "\t";
        // Each phrase: its reading, how many candidates it read and had, and those it had.
        for (const auto& phrase : phrases) {
            out << Narrow(phrase.reading) << ":" << phrase.read.size() << "/" << phrase.all.size();
            for (std::size_t c = 0; c < phrase.all.size(); ++c) out << (c ? "," : "=") << Narrow(phrase.all[c]);
            out << " | ";
        }
        out << "\n";
    }
}

// --context FILE (id, text written before it) and --context-previous (the
// rows before in the same group, 1-0 then 1-1): what each row follows,
// given to the composer as if just committed.
std::map<std::string, std::wstring> gContexts;
bool gContextPrevious = false;
// --added DIR (again for more): packs of words added after japanese-wikimedia
// (japanese-names, japanese-jmdict).
std::vector<std::filesystem::path> gAddedPacks;

int RunKeys(const ConnectionMatrix& matrix, const JapaneseDictionary& dictionary, const JapaneseConverter& converter,
            const std::filesystem::path& corpus, const std::filesystem::path& romaji,
            tekito::japanese::KeyConverter::Costs costs, std::size_t showMisses,
            const std::filesystem::path& dumpPhrases, const std::filesystem::path& writeSentences, bool byPhrase,
            bool useContext,
            const tekito::japanese::Loanwords* loanwords,
            const tekito::japanese::SentenceReranker* reranker,
            const tekito::japanese::KanaSlips* slips) {
    const auto table = tekito::japanese::RomajiTable::Load(romaji);
    if (!table) {
        std::cerr << "could not open the romaji table\n";
        return 1;
    }
    const tekito::japanese::KeyConverter keyConverter(dictionary, matrix, converter, *table, costs);
    tekito::japanese::JapaneseComposer composer(table.get());
    composer.SetConverter(&converter);
    // TEKITO_EVAL_PRODUCT=1: the settings the input method starts with
    // (UserSettings's defaults: digit and letter widths, number style,
    // special conversions), learning and the AI aside.
    if (const char* product = std::getenv("TEKITO_EVAL_PRODUCT"); product && *product == '1') {
        tekito::userdata::ApplyJapaneseSettings(composer, tekito::userdata::UserSettings{}, nullptr, false);
        UndoProductSettings(composer);
    }
    composer.SetNotationLevel(NotationLevel());
    composer.SetKeyConverter(&keyConverter);
    if (loanwords) composer.SetLoanwords(loanwords);
    composer.SetSentenceReranker(reranker);
    composer.SetAiJudge(gAiJudge, true);
    if (const char* features = std::getenv("TEKITO_EVAL_FEATURES"); features && *features) composer.SetKeepCheckedFeatures(true);
    // TEKITO_EVAL_SENTENCE_WEIGHTS=0: the sentence check without its learned weights.
    if (const char* weights = std::getenv("TEKITO_EVAL_SENTENCE_WEIGHTS"); weights && *weights == '0') {
        composer.SetSentenceWeights({.on = false});
    }
    // TEKITO_EVAL_WHOLES=N: how many whole texts with other phrase breaks
    // the sentence check reads too (the composer's default otherwise).
    if (const char* wholes = std::getenv("TEKITO_EVAL_WHOLES")) composer.SetWholeSentences(std::strtoul(wholes, nullptr, 10));
    if (const char* penalty = std::getenv("TEKITO_EVAL_WHOLE_PENALTY")) composer.SetWholePenalty(std::strtod(penalty, nullptr));
    if (const char* keep = std::getenv("TEKITO_EVAL_KEEP_NOTATION")) composer.SetKeepNotation(std::string_view(keep) != "0");
    // TEKITO_EVAL_KANJI=range[,grade]: the kanji range (KanjiRange: 1 常用+人名, 2 常用, 3 教育 up to the grade),
    // from the installed kanji-sets pack.
    static tekito::japanese::KanjiSets kanjiSets;
    if (const char* kanji = std::getenv("TEKITO_EVAL_KANJI")) {
        (void)kanjiSets.Load(tekito::ExternalLexiconProvider::PackDirectory(L"kanji-sets") / L"kanji.tsv");
        const int range = std::atoi(kanji);
        const char* comma = std::strchr(kanji, ',');
        composer.SetKanjiSets(&kanjiSets);
        composer.SetKanjiRange(static_cast<tekito::japanese::KanjiRange>(range), comma ? std::atoi(comma + 1) : 6);
    }
    // TEKITO_EVAL_NUMBER_STYLE=N: the number style (JapaneseComposer::NumberStyle, 0 as typed ... 5 official).
    if (const char* style = std::getenv("TEKITO_EVAL_NUMBER_STYLE")) {
        composer.SetNumberStyle(static_cast<tekito::japanese::JapaneseComposer::NumberStyle>(std::strtol(style, nullptr, 10)));
    }
    // TEKITO_EVAL_CHECK_ORDER=rank: the sentence check reads sentences put
    // together by rank, as before it took the converter's costs.
    if (const char* order = std::getenv("TEKITO_EVAL_CHECK_ORDER"); order && std::string_view(order) == "rank") {
        composer.SetSentencesByCost(false);
    }
    // TEKITO_EVAL_BEAM=width[,cost per nat[,margin[,whole cost]]]: the
    // sentence check searches phrase by phrase (SetSentenceBeam).
    if (const char* beam = std::getenv("TEKITO_EVAL_BEAM"); beam && *beam) {
        double values[4] = {0, 1000, 0, 0};
        const char* at = beam;
        for (double& value : values) {
            char* end = nullptr;
            value = std::strtod(at, &end);
            if (end == at || *end != ',') break;
            at = end + 1;
        }
        composer.SetSentenceBeam(static_cast<std::size_t>(values[0]), values[1], values[2], values[3]);
    }
    // TEKITO_EVAL_MIX_SLIPS=0: Mix's Japanese chunks without the slips pack.
    if (const char* mixSlips = std::getenv("TEKITO_EVAL_MIX_SLIPS"); mixSlips && *mixSlips == '0') {
        composer.SetMixSlips(false);
    }
    composer.SetKanaSlips(slips);

    // How many whole sentences are put together, and the counts reported.
    constexpr std::size_t kSentences = 50;
    constexpr std::size_t kTops[] = {1, 3, 5, 10, 20, 50};
    std::ofstream sentencesOut;
    if (!writeSentences.empty()) sentencesOut.open(writeSentences, std::ios::binary);
    struct KeyTotals {
        std::size_t rows{0}, exact{0}, inList{0}, errors{0}, characters{0};
        std::size_t top[std::size(kTops)]{};
        std::vector<double> microseconds;
        std::vector<double> listMicroseconds;
    };
    std::map<std::string, KeyTotals> totals;
    std::size_t shown = 0;
    // TEKITO_EVAL_KBEST=N: how often what was meant is among the converter's
    // N likeliest whole-reading texts (BestSentences), and how often only
    // there (not among the phrases' sentences).
    const char* kbestSetting = std::getenv("TEKITO_EVAL_KBEST");
    const std::size_t kbest = kbestSetting ? std::strtoul(kbestSetting, nullptr, 10) : 0;
    std::size_t kbestIn = 0, kbestOnly = 0, kbestRows = 0;
    // Live: how long each key takes to show, and the pause before Enter.
    std::vector<double> keyMicroseconds;
    std::vector<double> refineMicroseconds;
    const char* pauseSetting = std::getenv("TEKITO_EVAL_PAUSE");
    const bool pause = pauseSetting && std::string_view(pauseSetting) == "1";
    // --dump-phrases: the phrases of sentences that convert right, one per
    // row, as a keys file (for typing phrase by phrase).
    std::ofstream phrases;
    if (!dumpPhrases.empty()) phrases.open(dumpPhrases, std::ios::binary);
    // Keys files share the layout source, id, keys, expected...: LoadCorpus
    // reads keys where it reads readings.
    // TEKITO_EVAL_LIVE=1: live conversion, the text shown taken with Enter
    // (no Space); nothing is left to pick from afterwards.
    const char* liveSetting = std::getenv("TEKITO_EVAL_LIVE");
    const bool live = liveSetting && std::string_view(liveSetting) == "1";
    const char* chainSetting = std::getenv("TEKITO_EVAL_CHAIN");
    const bool chain = chainSetting && std::string_view(chainSetting) == "1";
    composer.SetLiveConversion(live);
    // TEKITO_EVAL_LIVE_OPEN=N: phrases kept open (0: none settled).
    if (const char* open = std::getenv("TEKITO_EVAL_LIVE_OPEN"); open && *open) {
        const auto count = std::stoul(open);
        composer.SetLiveOpenPhrases(count == 0 ? std::size_t{1} << 20 : count);
    }
    std::string previousGroup;
    std::wstring previousText;
    // TEKITO_EVAL_CONTEXTS=<readings file>: each row's text before it (the
    // readings file's context column), as the app passes the text before the
    // caret (SetPrecedingText).
    std::map<std::pair<std::string, std::string>, std::wstring> rowContexts;
    if (const char* contexts = std::getenv("TEKITO_EVAL_CONTEXTS"); contexts && *contexts) {
        std::ifstream in(contexts, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> fields;
            std::stringstream parts(line);
            for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
            if (fields.size() >= 3 && !fields[2].empty()) rowContexts[{fields[0], fields[1]}] = ja_eval::Widen(fields[2]);
        }
    }
    // --ai probe: per source, rows converted at once, and how often what was
    // meant is among the AI's first 1, 3, 5, 10 sentences and all it reads.
    constexpr std::size_t kProbeTops[] = {1, 3, 5, 10, SIZE_MAX};
    struct ProbeTotals {
        std::size_t rows{0}, once{0};
        std::size_t in[std::size(kProbeTops)]{};
        std::size_t missed[std::size(kMissNames)]{};
    };
    std::map<std::string, ProbeTotals> probeTotals;
    for (const auto& row : LoadCorpus(corpus)) {
        composer.Clear();
        gProbe.asked.clear();
        if (!gContexts.empty() || gContextPrevious) {
            const std::string group = row.id.substr(0, row.id.rfind('-'));
            std::wstring context;
            if (const auto found = gContexts.find(row.id); found != gContexts.end()) context = found->second;
            if (gContextPrevious && row.id.find('-') != std::string::npos && group == previousGroup) context = previousText;
            composer.SetRecentText(context);
            // As the app passes it: to the AI and to conversion and the sentence check,
            // and whether a sentence starts here.
            composer.SetSentenceStart(tekito::japanese::JapaneseComposer::EndsSentence(context));
            composer.SetPrecedingText(context);
            previousText = (group == previousGroup ? previousText : std::wstring()) +
                           (row.expected.empty() ? std::wstring() : row.expected.front());
            if (previousText.size() > 120) previousText.erase(0, previousText.size() - 120);
            previousGroup = group;
        }
        // Each row on its own: Enter (live) commits, which would lead the
        // next row on from it.
        if (live || !rowContexts.empty()) composer.ForgetContext();
        if (const auto found = rowContexts.find({row.source, row.id}); found != rowContexts.end()) {
            composer.SetPrecedingText(found->second);
        }
        for (const wchar_t key : row.reading) {
            const auto keyStart = Clock::now();
            composer.Insert(key);
            if (live) keyMicroseconds.push_back(std::chrono::duration<double, std::micro>(Clock::now() - keyStart).count());
        }
        // TEKITO_EVAL_PAUSE=1 (live): typing pauses before Enter.
        if (live && pause) {
            const auto pauseStart = Clock::now();
            composer.RefineLive();
            refineMicroseconds.push_back(std::chrono::duration<double, std::micro>(Clock::now() - pauseStart).count());
        }
        const auto start = Clock::now();
        if (!live) composer.Convert();
        const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        WriteShown(row.source, row.id, composer);
        std::wstring output = live ? composer.Commit() : composer.Preedit();
        if (byPhrase) {
            const auto pieces = composer.PhraseKeys();
            if (std::none_of(pieces.begin(), pieces.end(), [](const std::wstring& p) { return p.empty(); })) {
                composer.Clear();
                composer.ForgetContext();
                output.clear();
                for (const auto& piece : pieces) {
                    if (!useContext) composer.ForgetContext();
                    for (const wchar_t key : piece) composer.Insert(key);
                    composer.Convert();
                    output += composer.Commit();
                }
                composer.ForgetContext();
            }
        }
        WriteOutput(row.source, row.id, output);
        // TEKITO_EVAL_CHAIN=1: each row is committed and the next row goes on
        // from it, as typing on right after a commit does.
        if (chain && !live) {
            composer.Commit();
        }
        WriteFeatures(row.source, row.id, composer.LastCheckedSentences(), row.expected);
        if (gAiJudge == &gProbe && gProbe.asked.size() == 1) WriteChecked(row.source, row.id, composer.LastCheckedSentences(), row.expected);
        if (gAiJudge == &gProbe) {
            for (auto* t : {&probeTotals[row.source], &probeTotals["all"]}) {
                ++t->rows;
                // Only a row converted at once: its sentences are the whole row's.
                if (gProbe.asked.size() != 1) continue;
                ++t->once;
                const auto& asked = gProbe.asked.front();
                std::size_t at = SIZE_MAX;
                for (std::size_t k = 0; k < asked.size() && at == SIZE_MAX; ++k) {
                    if (std::find(row.expected.begin(), row.expected.end(), asked[k]) != row.expected.end()) at = k;
                }
                if (t == &probeTotals["all"]) WriteAsked(row.source, row.id, asked, at);
                if (at == SIZE_MAX && !row.expected.empty()) {
                    // The likeliest reason among the ways it may be written.
                    std::wstring reading;
                    for (const auto& phrase : composer.LastChecked()) reading += phrase.reading;
                    Miss why = Miss::Dictionary;
                    for (const auto& meant : row.expected) {
                        why = std::min(why, WhyMissed(converter, reading, composer.LastChecked(), meant));
                    }
                    ++t->missed[static_cast<int>(why)];
                    if (t == &probeTotals["all"]) WriteMiss(row.source, row.id, why, reading, row.expected.front(), output, composer.LastChecked());
                }
                for (std::size_t n = 0; n < std::size(kProbeTops); ++n) {
                    if (at != SIZE_MAX && at < kProbeTops[n]) ++t->in[n];
                }
            }
        }
        std::size_t distance = SIZE_MAX, length = 0;
        for (const auto& expected : row.expected) {
            const std::size_t d = EditDistance(output, expected);
            if (d < distance) {
                distance = d;
                length = expected.size();
            }
        }
        if (phrases.is_open() && distance == 0 && !byPhrase) {
            const auto segments = composer.Segments();
            const auto keys = composer.PhraseKeys();
            for (std::size_t p = 0; p < segments.size() && p < keys.size(); ++p) {
                if (keys[p].empty()) continue;
                phrases << "phrase-" << row.source << "\t" << row.id << "-" << p << "\t" << Narrow(keys[p]) << "\t"
                        << Narrow(segments[p].text) << "\n";
            }
        }
        // One pick away: what was meant is a phrase's first nine candidates
        // (the list the second Space opens) with the other phrases kept.
        // The second Space: the list opens for the first phrase.
        const auto listStart = Clock::now();
        composer.NextCandidate();
        const double listElapsed = std::chrono::duration<double, std::micro>(Clock::now() - listStart).count();
        composer.PreviousCandidate();
        bool inList = distance == 0;
        const std::size_t phraseCount = byPhrase ? 0 : composer.Segments().size();
        for (std::size_t f = 0; !inList && f < phraseCount; ++f) {
            composer.MoveFocus(-static_cast<int>(phraseCount));
            composer.MoveFocus(static_cast<int>(f));
            composer.NextCandidate();
            composer.PreviousCandidate();
            const auto segments = composer.Segments();
            const auto* candidates = composer.FocusedCandidates();
            for (std::size_t j = 0; candidates && j < std::min<std::size_t>(9, candidates->size()) && !inList; ++j) {
                std::wstring text;
                // A whole-input candidate is the whole text.
                for (std::size_t s = 0; s < segments.size() && (*candidates)[j].reading.empty(); ++s) {
                    text += s == f ? (*candidates)[j].text : segments[s].text;
                }
                if (!(*candidates)[j].reading.empty()) text = (*candidates)[j].text;
                inList = std::find(row.expected.begin(), row.expected.end(), text) != row.expected.end();
            }
        }
        // TEKITO_EVAL_LIST_DUMP=1: for a row not on the lists' first pages,
        // where in the first phrase's whole list what was meant is (or -1).
        if (const char* dump = std::getenv("TEKITO_EVAL_LIST_DUMP"); dump && *dump == '1' && !inList && phraseCount) {
            composer.MoveFocus(-static_cast<int>(phraseCount));
            composer.NextCandidate();
            composer.PreviousCandidate();
            const auto* candidates = composer.FocusedCandidates();
            int at = -1;
            std::wstring all;
            for (std::size_t j = 0; candidates && j < candidates->size(); ++j) {
                if (std::find(row.expected.begin(), row.expected.end(), (*candidates)[j].text) != row.expected.end() &&
                    at < 0) {
                    at = static_cast<int>(j);
                }
                if (j < 30) all += (j ? L" " : L"") + (*candidates)[j].text;
            }
            std::cout << "list " << row.source << ":" << row.id << " at=" << at << " size="
                      << (candidates ? candidates->size() : 0) << " phrases=" << phraseCount << "  " << Narrow(all)
                      << "\n";
        }
        // Whole sentences from the phrases' lists, likeliest first: where
        // what was meant is among them (for choosing again with a model).
        std::size_t sentenceRank = SIZE_MAX;
        if (!byPhrase) {
            std::vector<std::vector<std::wstring>> lists;
            std::vector<std::vector<std::int64_t>> deltas;
            std::vector<std::wstring> wholes;
            for (std::size_t f = 0; f < phraseCount; ++f) {
                composer.MoveFocus(-static_cast<int>(phraseCount));
                composer.MoveFocus(static_cast<int>(f));
                composer.NextCandidate();
                composer.PreviousCandidate();
                const auto* candidates = composer.FocusedCandidates();
                lists.emplace_back();
                deltas.emplace_back();
                std::int64_t first = 0;
                for (std::size_t j = 0; candidates && j < candidates->size() && lists.back().size() < 20; ++j) {
                    const auto& candidate = (*candidates)[j];
                    // A whole-input candidate is a sentence of its own.
                    if (!candidate.reading.empty()) {
                        wholes.push_back(candidate.text);
                    } else if (std::find(lists.back().begin(), lists.back().end(), candidate.text) ==
                               lists.back().end()) {
                        // Kana, typed forms and the like have no cost of
                        // their own: KanaCost over the first. The list's
                        // order stays (each delta at least the one before).
                        const bool costed = candidate.kind == tekito::japanese::PhraseCandidate::Kind::Dictionary &&
                                            candidate.cost > 0 && candidate.cost < (std::int64_t{1} << 30);
                        if (lists.back().empty()) first = costed ? candidate.cost : 0;
                        std::int64_t delta = lists.back().empty() ? 0 : costed ? candidate.cost - first : KanaCost();
                        if (!deltas.back().empty()) delta = std::max(delta, deltas.back().back());
                        lists.back().push_back(candidate.text);
                        deltas.back().push_back(delta);
                    }
                }
            }
            auto sentences = SentencesByCostChosen() ? SentencesByCost(lists, deltas, kSentences)
                                                     : SentencesByRank(lists, kSentences);
            for (auto& whole : wholes) {
                if (std::find(sentences.begin(), sentences.end(), whole) == sentences.end()) sentences.push_back(whole);
            }
            for (std::size_t s = 0; s < sentences.size() && sentenceRank == SIZE_MAX; ++s) {
                if (std::find(row.expected.begin(), row.expected.end(), sentences[s]) != row.expected.end()) sentenceRank = s;
            }
            if (kbest > 0) {
                ++kbestRows;
                bool in = false;
                for (const auto& guess : converter.BestSentences(composer.TypedReading(), kbest)) {
                    in = in || std::find(row.expected.begin(), row.expected.end(), guess.text) != row.expected.end();
                }
                kbestIn += in;
                kbestOnly += in && sentenceRank == SIZE_MAX;
            }
            if (sentencesOut.is_open()) {
                for (std::size_t s = 0; s < sentences.size(); ++s) {
                    const bool meant = std::find(row.expected.begin(), row.expected.end(), sentences[s]) != row.expected.end();
                    sentencesOut << row.source << "\t" << row.id << "\t" << s << "\t" << (meant ? 1 : 0) << "\t"
                                 << Narrow(sentences[s]) << "\n";
                }
            }
        }
        for (KeyTotals* t : {&totals[row.source], &totals["all"]}) {
            ++t->rows;
            if (distance == 0) ++t->exact;
            if (inList) ++t->inList;
            for (std::size_t n = 0; n < std::size(kTops); ++n) {
                if (sentenceRank < kTops[n]) ++t->top[n];
            }
            t->listMicroseconds.push_back(listElapsed);
            t->errors += distance;
            t->characters += length;
            t->microseconds.push_back(elapsed);
        }
        if (distance != 0 && shown < showMisses) {
            ++shown;
            std::cout << "miss " << row.source << ":" << row.id << "  " << Narrow(output) << "  (want "
                      << Narrow(row.expected.front()) << (inList ? ", in the list" : "") << ")\n";
            if (std::getenv("TEKITO_EVAL_LISTS")) {
                // Each phrase's first choices, to see why the meant text was not among them.
                const std::size_t count = composer.Segments().size();
                for (std::size_t f = 0; f < count; ++f) {
                    composer.MoveFocus(-static_cast<int>(count));
                    composer.MoveFocus(static_cast<int>(f));
                    composer.NextCandidate();
                    composer.PreviousCandidate();
                    std::cout << "    phrase " << f << ":";
                    const auto* candidates = composer.FocusedCandidates();
                    for (std::size_t j = 0; candidates && j < std::min<std::size_t>(9, candidates->size()); ++j) {
                        std::cout << " " << Narrow((*candidates)[j].text);
                    }
                    std::cout << "\n";
                }
            }
        }
    }
    std::cout << std::fixed << std::setprecision(1);
    if (kbestRows > 0) {
        std::cout << "kbest" << kbest << ": in=" << 100.0 * kbestIn / kbestRows << "% only_there="
                  << 100.0 * kbestOnly / kbestRows << "%\n";
    }
    for (const auto& [source, t] : totals) {
        std::cout << source << ": rows=" << t.rows << " top1=" << 100.0 * t.exact / t.rows << "%"
                  << " in_list=" << 100.0 * t.inList / t.rows << "%";
        if (!byPhrase) {
            std::cout << " sentences";
            for (std::size_t n = 0; n < std::size(kTops); ++n) {
                std::cout << " top" << kTops[n] << "=" << 100.0 * t.top[n] / t.rows << "%";
            }
        }
        std::cout
                  << " cer=" << (t.characters ? 100.0 * t.errors / t.characters : 0.0) << "%"
                  << " convert_us_p50=" << Percentile(t.microseconds, 0.5)
                  << " p95=" << Percentile(t.microseconds, 0.95)
                  << " max=" << Percentile(t.microseconds, 1.0)
                  << " list_us_p50=" << Percentile(t.listMicroseconds, 0.5)
                  << " p95=" << Percentile(t.listMicroseconds, 0.95)
                  << " max=" << Percentile(t.listMicroseconds, 1.0) << "\n";
    }
    for (const auto& [source, t] : probeTotals) {
        std::cout << "probe " << source << ": rows=" << t.rows << " once=" << t.once;
        for (std::size_t n = 0; n < std::size(kProbeTops); ++n) {
            std::cout << (kProbeTops[n] == SIZE_MAX ? std::string(" in_all=") : " in" + std::to_string(kProbeTops[n]) + "=")
                      << (t.once ? 100.0 * t.in[n] / t.once : 0.0) << "%";
        }
        for (std::size_t n = 0; n < std::size(kMissNames); ++n) {
            std::cout << " " << kMissNames[n] << "=" << (t.once ? 100.0 * t.missed[n] / t.once : 0.0) << "%";
        }
        std::cout << "\n";
    }
    if (live) {
        std::cout << "live keys: us_p50=" << Percentile(keyMicroseconds, 0.5) << " p95=" << Percentile(keyMicroseconds, 0.95)
                  << " p99=" << Percentile(keyMicroseconds, 0.99) << " max=" << Percentile(keyMicroseconds, 1.0) << "\n";
        if (pause) {
            std::cout << "live pause: us_p50=" << Percentile(refineMicroseconds, 0.5)
                      << " p95=" << Percentile(refineMicroseconds, 0.95) << " p99=" << Percentile(refineMicroseconds, 0.99)
                      << " max=" << Percentile(refineMicroseconds, 1.0) << "\n";
        }
    }
    return 0;
}


// --chunks: Japanese and English typed in chunks, a Space after each, with
// the composer in mixed typing (ChunkJudge): each chunk is converted with
// Space and committed by the next. A chunk ending in "*" in the corpus is
// English (the answer, not typed).
//
// --japanese-mode types the chunks in Japanese mode instead (no judging);
// --no-slips leaves slips in the keys as typed; --english-engine spells
// English chunks as English Auto does (TEKITO_DATA_PACK_DIR: its packs); --write-chunks <path>
// writes each chunk's committed text and what one pick from its list gives
// (source, id, chunk, committed, choices split by U+001F), to compare text
// typed with slips against the same text typed right
// (eval/typo-study.py).
struct ChunkOptions {
    std::filesystem::path englishScores;
    std::filesystem::path pack;
    tekito::japanese::ChunkJudge::Thresholds thresholds;
    std::size_t showChunks{0};
    bool japaneseMode{false};
    bool slips{true};
    bool englishEngine{false};
    // English typed into Japanese without spaces is looked for (--no-run-together: not).
    bool runTogether{true};
    // Also judge each sentence as a whole (ChunkJudge::JudgeSentence) and
    // count the sentences it gets right.
    bool sentenceJudge{false};
    // Mix as typed since it holds the sentence: each chunk converted with
    // Space, the next typed on, the whole committed with Enter at the end.
    bool hold{false};
    std::filesystem::path writeChunks;
    // --reranker, --slips: the sentence check and the slip finder.
    std::filesystem::path rerankerPack;
    std::filesystem::path slipsPack;
};

// The text of the composition with one phrase's choice replaced, for each
// of the first nine choices of each phrase (the list the second Space opens).
std::vector<std::wstring> OnePickAway(tekito::japanese::JapaneseComposer& composer) {
    std::vector<std::wstring> texts;
    const std::size_t count = composer.Segments().size();
    for (std::size_t f = 0; f < count; ++f) {
        composer.MoveFocus(-static_cast<int>(count));
        composer.MoveFocus(static_cast<int>(f));
        composer.NextCandidate();
        composer.PreviousCandidate();
        const auto segments = composer.Segments();
        const auto* candidates = composer.FocusedCandidates();
        for (std::size_t j = 0; candidates && j < std::min<std::size_t>(9, candidates->size()); ++j) {
            std::wstring text;
            if (!(*candidates)[j].reading.empty()) {
                text = (*candidates)[j].text;
            } else {
                for (std::size_t s = 0; s < segments.size(); ++s) text += s == f ? (*candidates)[j].text : segments[s].text;
            }
            texts.push_back(std::move(text));
        }
    }
    composer.MoveFocus(-static_cast<int>(count));
    return texts;
}

int RunChunks(const ConnectionMatrix& matrix, const JapaneseDictionary& dictionary, const JapaneseConverter& converter,
              const std::filesystem::path& corpus, const std::filesystem::path& romaji,
              tekito::japanese::KeyConverter::Costs costs, std::size_t showMisses, const ChunkOptions& options,
              const tekito::japanese::Loanwords* names) {
    const auto table = tekito::japanese::RomajiTable::Load(romaji);
    if (!table) {
        std::cerr << "could not open the romaji table\n";
        return 1;
    }
    const tekito::ExternalFrequencyProvider english(options.englishScores);
    if (!english.IsLoaded()) {
        std::cerr << "could not open the English word scores\n";
        return 1;
    }
    const tekito::japanese::KeyConverter keyConverter(dictionary, matrix, converter, *table, costs);
    tekito::japanese::JapaneseComposer composer(table.get());
    composer.SetConverter(&converter);
    // TEKITO_EVAL_PRODUCT=1: the settings the input method starts with
    // (UserSettings's defaults: digit and letter widths, number style,
    // special conversions), learning and the AI aside.
    if (const char* product = std::getenv("TEKITO_EVAL_PRODUCT"); product && *product == '1') {
        tekito::userdata::ApplyJapaneseSettings(composer, tekito::userdata::UserSettings{}, nullptr, !options.japaneseMode);
        UndoProductSettings(composer);
    }
    composer.SetNotationLevel(NotationLevel());
    if (options.slips) composer.SetKeyConverter(&keyConverter);
    if (names) composer.SetLoanwords(names);
    if (const char* mixed = std::getenv("TEKITO_EVAL_MIXED"); mixed && *mixed) composer.SetKeepMixedSentences(256);
    tekito::japanese::SentenceReranker reranker;
    if (!options.rerankerPack.empty() && !reranker.Open(options.rerankerPack)) {
        std::cerr << "could not open the reranker at " << options.rerankerPack.string() << "\n";
        return 1;
    }
    if (reranker.IsOpen()) composer.SetSentenceReranker(&reranker);
    // --ai: the model chooses among the sentence's texts on Enter (--hold);
    // TEKITO_EVAL_MIX_AI_MARGIN=nats, TEKITO_EVAL_MIX_AI_NOTATION=0 lets it
    // take a text that only writes the first otherwise; TEKITO_EVAL_MIX_AI_COUNT=N
    // texts read (16).
    if (gAiJudge) {
        composer.SetAiJudge(gAiJudge, true);
        const char* margin = std::getenv("TEKITO_EVAL_MIX_AI_MARGIN");
        const char* notation = std::getenv("TEKITO_EVAL_MIX_AI_NOTATION");
        const char* count = std::getenv("TEKITO_EVAL_MIX_AI_COUNT");
        composer.SetMixAi(true, margin && *margin ? std::strtod(margin, nullptr) : 4.0, !(notation && *notation == '0'),
                          count && *count ? std::strtoul(count, nullptr, 10) : 16);
    }
    // TEKITO_EVAL_CHECK_ORDER=rank: the sentence check reads sentences put
    // together by rank, as before it took the converter's costs.
    if (const char* order = std::getenv("TEKITO_EVAL_CHECK_ORDER"); order && std::string_view(order) == "rank") {
        composer.SetSentencesByCost(false);
    }
    if (const char* mixSlips = std::getenv("TEKITO_EVAL_MIX_SLIPS"); mixSlips && *mixSlips == '0') {
        composer.SetMixSlips(false);
    }
    tekito::japanese::KanaSlips kanaSlips;
    if (!options.slipsPack.empty() && !kanaSlips.Open(options.slipsPack)) {
        std::cerr << "could not open the slip finder at " << options.slipsPack.string() << "\n";
        return 1;
    }
    if (kanaSlips.IsOpen()) composer.SetKanaSlips(&kanaSlips);
    const tekito::japanese::ChunkJudge judge(*table, converter, options.slips ? &keyConverter : nullptr, english, names,
                                             options.thresholds);
    // The written-forms pack beside the English data (TEKITO_DATA_PACK_DIR).
    tekito::japanese::WrittenForms writtenForms;
    if (!std::getenv("TEKITO_EVAL_NO_WRITTEN_FORMS") &&
        writtenForms.Open(tekito::ExternalLexiconProvider::PackDirectory(L"written-forms"))) {
        const_cast<tekito::japanese::ChunkJudge&>(judge).SetWrittenForms(&writtenForms);
    }
    // english-names beside it too (TEKITO_EVAL_NO_ENGLISH_NAMES=1: not).
    tekito::japanese::WrittenForms englishNames;
    if (!std::getenv("TEKITO_EVAL_NO_ENGLISH_NAMES") &&
        englishNames.Open(tekito::ExternalLexiconProvider::PackDirectory(L"english-names"))) {
        const_cast<tekito::japanese::ChunkJudge&>(judge).SetEnglishNames(&englishNames);
    }
    // product-names too (TEKITO_EVAL_NO_PRODUCT_NAMES=1: not).
    tekito::japanese::WrittenForms productNames;
    if (!std::getenv("TEKITO_EVAL_NO_PRODUCT_NAMES") &&
        productNames.Open(tekito::ExternalLexiconProvider::PackDirectory(L"product-names"))) {
        const_cast<tekito::japanese::ChunkJudge&>(judge).SetProductNames(&productNames);
    }
    // japanese-letter-odds beside japanese-core (TEKITO_EVAL_NO_LETTER_ODDS=1: not).
    tekito::japanese::LetterOdds letterOdds;
    if (!std::getenv("TEKITO_EVAL_NO_LETTER_ODDS") && letterOdds.Open(options.pack.parent_path() / "japanese-letter-odds")) {
        const_cast<tekito::japanese::ChunkJudge&>(judge).SetLetterOdds(&letterOdds);
    }
    // TEKITO_EVAL_ENGLISH_AROUND=0: chunks that read both ways not judged
    // by the English around them (ChunkJudge::SetEnglishAround).
    // TEKITO_EVAL_MIX_LIVE=1: Mix live conversion on, each key's display timed.
    const bool mixLive = [] {
        const char* value = std::getenv("TEKITO_EVAL_MIX_LIVE");
        return value && *value == '1';
    }();
    if (mixLive) composer.SetMixLiveConversion(true);
    std::vector<double> keyMicroseconds;
    if (const char* around = std::getenv("TEKITO_EVAL_ENGLISH_AROUND"); around && *around == '0') {
        const_cast<tekito::japanese::ChunkJudge&>(judge).SetEnglishAround(false);
    }
    // TEKITO_EVAL_PARTICLE_AFTER=0: ChunkJudge::SetParticleAfter(false).
    if (const char* after = std::getenv("TEKITO_EVAL_PARTICLE_AFTER"); after && *after == '0') {
        const_cast<tekito::japanese::ChunkJudge&>(judge).SetParticleAfter(false);
    }
    const auto parts = tekito::japanese::UserPartsOfSpeech::Load(options.pack / "pos.tsv");
    const auto noun = parts.For(tekito::japanese::UserWordKind::ProperNoun);
    if (!options.japaneseMode) composer.SetMixedTyping(&judge, noun ? noun->right : 0);
    if (options.runTogether) composer.SetRunTogether(&judge);
    // TEKITO_EVAL_SPECIAL=1: dates, symbols and emoji by their readings, as
    // the text service offers them (the special-conversions pack beside the
    // English data), the first symbol on the first page.
    if (const char* special = std::getenv("TEKITO_EVAL_SPECIAL"); special && *special == '1') {
        composer.SetSpecialConversions(&tekito::SpecialConversions::Installed());
        composer.SetEmojiOnFirstPage(true);
    }
    std::unique_ptr<tekito::IConversionEngine> englishEngine;
    std::unique_ptr<tekito::EnglishSpelling> spelling;
    if (options.englishEngine) {
        tekito::PreloadDefaultEngineData();
        englishEngine = tekito::CreateDefaultConversionEngine();
        if (!englishEngine) {
            std::cerr << "could not load English Auto (TEKITO_DATA_PACK_DIR)\n";
            return 1;
        }
        spelling = std::make_unique<tekito::EnglishSpelling>(*englishEngine);
        composer.SetEnglishSpelling(spelling.get());
    }
    std::ofstream written;
    if (!options.writeChunks.empty()) written.open(options.writeChunks, std::ios::binary);

    struct ChunkTotals {
        std::size_t rows{0}, exact{0}, errors{0}, characters{0};
        std::size_t chunks{0}, english{0}, englishRight{0}, japanese{0}, japaneseRight{0};
        // Sentences whose every chunk was taken as the right language.
        std::size_t judgedRows{0};
        // Without --hold: chunks whose list (first nine) has what the
        // sentence needs there, how far down it is (the picks it takes),
        // and sentences every chunk of which can be put right so.
        std::size_t listedChunks{0}, reachedChunks{0}, picks{0}, reachedRows{0};
        std::size_t rank[9]{};
    };
    std::map<std::string, ChunkTotals> totals;
    std::map<std::string, std::size_t> sentenceRows, sentenceRight;
    std::size_t shownMisses = 0, shownChunks = 0;
    for (auto row : LoadCorpus(corpus)) {
        // An English word starting the sentence may take a capital, as Mix
        // gives it one (Dataを for dataを): both are right.
        for (std::size_t e = 0, n = row.expected.size(); e < n; ++e) {
            std::wstring capital = row.expected[e];
            if (!capital.empty() && capital.front() >= L'a' && capital.front() <= L'z') {
                capital.front() = static_cast<wchar_t>(capital.front() - L'a' + L'A');
                if (std::find(row.expected.begin(), row.expected.end(), capital) == row.expected.end()) {
                    row.expected.push_back(std::move(capital));
                }
            }
        }
        std::wstring output;
        std::size_t englishBefore = 0;
        std::wstring englishText;
        bool japaneseInSentence = false;
        composer.ForgetContext();
        composer.SetSentenceStart(true);
        std::vector<std::pair<bool, bool>> judged;  // English meant, English said
        std::wistringstream words(row.reading);
        std::size_t chunkIndex = 0;
        std::vector<std::optional<std::size_t>> chunkRanks;
        std::wstring fixed;  // the output with each chunk put right where its list can
        for (std::wstring chunk; words >> chunk; ++chunkIndex) {
            const bool meant = chunk.back() == L'*';
            if (meant) chunk.pop_back();
            // As the composer does: punctuation after the word is not judged.
            std::wstring word = chunk;
            while (!word.empty() && std::wstring_view(L".,!?").find(word.back()) != std::wstring_view::npos) word.pop_back();
            const auto judgement = word.empty() ? tekito::japanese::ChunkJudge::Judgement{englishBefore > 0, chunk, false}
                                                : judge.Judge(word, englishBefore, japaneseInSentence,
                                                              spelling.get(), englishText);
            const bool said = judgement.english;
            englishBefore = said ? englishBefore + 1 : 0;
            if (!said) japaneseInSentence = true;
            if (std::wstring_view(L".!?").find(chunk.back()) != std::wstring_view::npos) japaneseInSentence = false;
            judged.push_back({meant, said});
            if (shownChunks < options.showChunks) {
                ++shownChunks;
                std::cout << "chunk " << Narrow(chunk) << (meant ? " E" : " J") << " said=" << (said ? "E" : "J")
                          << " written=" << Narrow(judgement.written) << (judgement.either ? " either" : "")
                          << " in_japanese=" << (names && names->InJapaneseText(chunk) ? *names->InJapaneseText(chunk) : -9.0)
                          << (meant != said ? "  WRONG" : "") << "\n";
            }
            for (const wchar_t key : chunk) {
                const auto keyStart = std::chrono::steady_clock::now();
                composer.Insert(key);
                if (mixLive) {
                    (void)composer.Segments();
                    keyMicroseconds.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - keyStart).count());
                }
            }
            composer.Convert();
            if (options.hold) continue;
            const auto choices = OnePickAway(composer);
            // TEKITO_EVAL_DUMP_LISTS=1: each chunk's list as shown.
            if (const char* dump = std::getenv("TEKITO_EVAL_DUMP_LISTS"); dump && *dump == '1') {
                std::cout << "list " << row.source << ":" << row.id << " " << Narrow(chunk) << ":";
                for (std::size_t c = 0; c < choices.size(); ++c) std::cout << " [" << c + 1 << "]" << Narrow(choices[c]);
                std::cout << "\n";
            }
            // Where in this chunk's list the text the sentence goes on with
            // is: the first choice that keeps the output a start of an
            // acceptable text (spaces left out).
            std::optional<std::size_t> at;
            {
                const auto squeeze = [](std::wstring text) {
                    std::erase(text, L' ');
                    return text;
                };
                for (std::size_t c = 0; c < choices.size() && !at; ++c) {
                    std::wstring so = fixed + choices[c];
                    so = squeeze(so);
                    for (const auto& expected : row.expected) {
                        if (squeeze(expected).starts_with(so)) {
                            at = c;
                            break;
                        }
                    }
                }
                chunkRanks.push_back(at);
            }
            // TEKITO_EVAL_PICK=1: the choice is picked before Enter, as the
            // typist would, so the next chunks go on from it (top1 then counts
            // what the picks make).
            static const bool pick = [] {
                const char* value = std::getenv("TEKITO_EVAL_PICK");
                return value && *value == '1';
            }();
            const std::size_t segmentCount = composer.Segments().size();
            if (pick && at && *at > 0 && *at < 9 && segmentCount > 0) {
                composer.MoveFocus(-static_cast<int>(segmentCount));
                composer.ShowCandidates();
                composer.SelectCandidate(*at);
            }
            const auto committed = composer.Commit();
            fixed += at ? choices[*at] : committed;
            if (written.is_open()) {
                written << row.source << '\t' << row.id << '\t' << chunkIndex << '\t' << Narrow(committed) << '\t';
                for (std::size_t c = 0; c < choices.size(); ++c) {
                    if (c) written << '\x1f';
                    written << Narrow(choices[c]);
                }
                written << '\n';
            }
            if (composer.TakeSpaceBefore() && !committed.empty() && output.ends_with(L' ')) output.pop_back();
            output += committed;
            if (said) {
                englishText += committed;
            } else {
                englishText.clear();
            }
        }
        if (options.hold) {
            WriteShown(row.source, row.id, composer);
            output = composer.Commit();
            // TEKITO_EVAL_SETTLED=<path>: the sentence as its chunks were
            // shown when each settled against what Enter made of it (source,
            // id, shown right 1/0, committed right 1/0, shown, committed).
            static std::ofstream settledOut = [] {
                const char* path = std::getenv("TEKITO_EVAL_SETTLED");
                return path && *path ? std::ofstream(path, std::ios::binary) : std::ofstream();
            }();
            if (settledOut.is_open()) {
                const auto trimmed = [](std::wstring text) {
                    while (!text.empty() && text.back() == L' ') text.pop_back();
                    return text;
                };
                const auto right = [&](const std::wstring& text) {
                    return std::find(row.expected.begin(), row.expected.end(), text) != row.expected.end();
                };
                const auto shown = trimmed(composer.LastShownSentence()), committed = trimmed(output);
                settledOut << row.source << '\t' << row.id << '\t' << right(shown) << '\t' << right(committed) << '\t'
                           << Narrow(shown) << '\t' << Narrow(committed) << '\n';
            }
            // TEKITO_EVAL_MIXED=<prefix>[:N]: the sentence's texts with other
            // choices of its chunks (<prefix>.sentences.tsv: source, id, k,
            // meant 1/0, text) and the keys typed (<prefix>.keys.tsv: id,
            // keys), for an AI model to choose among offline.
            static std::ofstream mixedOut, mixedKeys;
            static const std::size_t mixedCount = [&] {
                const char* value = std::getenv("TEKITO_EVAL_MIXED");
                if (!value || !*value) return std::size_t{0};
                std::string prefix = value;
                std::size_t count = 24;
                if (const auto colon = prefix.rfind(':'); colon != std::string::npos && colon > 1) {
                    count = std::stoul(prefix.substr(colon + 1));
                    prefix.resize(colon);
                }
                mixedOut.open(prefix + ".sentences.tsv", std::ios::binary);
                mixedKeys.open(prefix + ".keys.tsv", std::ios::binary);
                return count;
            }();
            if (mixedCount > 0) {
                const auto& texts = composer.LastMixedSentences();
                for (std::size_t k = 0; k < std::min(texts.size(), mixedCount); ++k) {
                    auto text = texts[k];
                    while (!text.empty() && text.back() == L' ') text.pop_back();
                    const bool meant = std::find(row.expected.begin(), row.expected.end(), text) != row.expected.end();
                    mixedOut << row.source << '\t' << row.id << '\t' << k << '\t' << (meant ? 1 : 0) << '\t' << Narrow(text) << '\n';
                }
                mixedKeys << row.id << '\t' << Narrow(composer.LastMixedKeys()) << '\n';
            }
        }
        // English last leaves its space after it.
        while (!output.empty() && output.back() == L' ') output.pop_back();
        const auto [distance, length] = ja_eval::Closest(output, row.expected);
        const bool allJudged = std::all_of(judged.begin(), judged.end(),
                                           [](const auto& pair) { return pair.first == pair.second; });
        if (options.sentenceJudge) {
            std::vector<std::wstring> keys;
            std::vector<bool> meant;
            std::wistringstream again(row.reading);
            for (std::wstring chunk; again >> chunk;) {
                meant.push_back(chunk.back() == L'*');
                if (meant.back()) chunk.pop_back();
                while (!chunk.empty() && std::wstring_view(L".,!?").find(chunk.back()) != std::wstring_view::npos) chunk.pop_back();
                keys.push_back(chunk);
            }
            const auto whole = judge.JudgeSentence(keys, spelling.get());
            bool right = true;
            for (std::size_t i = 0; i < whole.size(); ++i) {
                // Punctuation alone is as before.
                if (keys[i].empty()) continue;
                right = right && whole[i].english == meant[i];
            }
            // TEKITO_EVAL_WHOLE_CHUNKS=1: each chunk as the whole sentence
            // judges it (as the chunk lines show the left-to-right calls).
            if (const char* each = std::getenv("TEKITO_EVAL_WHOLE_CHUNKS"); each && *each == '1') {
                for (std::size_t i = 0; i < whole.size(); ++i) {
                    std::cout << "whole " << Narrow(keys[i]) << (meant[i] ? " E" : " J")
                              << " said=" << (whole[i].english ? "E" : "J") << (whole[i].either ? " either" : "") << "\n";
                }
            }
            for (const auto* name : {row.source.c_str(), "all"}) {
                ++sentenceRows[name];
                if (right) ++sentenceRight[name];
            }
            if (right != allJudged && shownChunks < options.showChunks) {
                std::cout << (right ? "BETTER " : "WORSE  ") << Narrow(row.reading) << "\n";
            }
        }
        WriteOutput(row.source, row.id, output);
        for (ChunkTotals* t : {&totals[row.source], &totals["all"]}) {
            ++t->rows;
            if (distance == 0) ++t->exact;
            if (allJudged) ++t->judgedRows;
            if (!options.hold) {
                bool reached = true;
                for (const auto& at : chunkRanks) {
                    ++t->listedChunks;
                    if (!at) {
                        reached = false;
                        continue;
                    }
                    ++t->reachedChunks;
                    t->picks += *at;
                    if (*at < std::size(t->rank)) ++t->rank[*at];
                }
                if (reached) ++t->reachedRows;
            }
            t->errors += distance;
            t->characters += length;
            for (const auto& [meant, said] : judged) {
                ++t->chunks;
                if (meant) {
                    ++t->english;
                    if (said) ++t->englishRight;
                } else {
                    ++t->japanese;
                    if (!said) ++t->japaneseRight;
                }
            }
        }
        if (distance != 0 && shownMisses < showMisses) {
            ++shownMisses;
            std::cout << "miss " << row.source << ":" << row.id << "  " << Narrow(output) << "  (want "
                      << Narrow(row.expected.front()) << ")\n";
        }
    }
    std::cout << std::fixed << std::setprecision(1);
    for (const auto& [source, t] : totals) {
        std::cout << source << ": rows=" << t.rows << " top1=" << 100.0 * t.exact / t.rows << "%"
                  << " judged_rows=" << 100.0 * t.judgedRows / t.rows << "%"
                  << (options.hold ? std::string{}
                                   : " in_lists=" + std::to_string(100.0 * t.reachedRows / std::max<std::size_t>(t.rows, 1)) +
                                         "% chunk_rank1/2/3=" + std::to_string(t.rank[0]) + "/" +
                                         std::to_string(t.rank[1]) + "/" + std::to_string(t.rank[2]) +
                                         " missing=" + std::to_string(t.listedChunks - t.reachedChunks) +
                                         " picks=" + std::to_string(t.picks))
                  << " cer=" << (t.characters ? 100.0 * t.errors / t.characters : 0.0) << "%"
                  << " english_chunks=" << t.english << " judged_english="
                  << (t.english ? 100.0 * t.englishRight / t.english : 0.0) << "%"
                  << " japanese_chunks=" << t.japanese << " judged_japanese="
                  << (t.japanese ? 100.0 * t.japaneseRight / t.japanese : 0.0) << "%\n";
    }
    for (const auto& [source, rows] : sentenceRows) {
        std::cout << source << ": sentence_judged_rows=" << 100.0 * sentenceRight[source] / rows << "%\n";
    }
    if (!keyMicroseconds.empty()) {
        std::cout << "mix live keys=" << keyMicroseconds.size() << " p50_ms=" << Percentile(keyMicroseconds, 0.5) / 1000
                  << " p95_ms=" << Percentile(keyMicroseconds, 0.95) / 1000
                  << " max_ms=" << Percentile(keyMicroseconds, 1.0) / 1000 << "\n";
    }
    return 0;
}

// --dump-lattices <gen.tsv> --dump-out <path> [--dump-shard i/n]: for training
// a model that scores the converter's words (scripts/ja/train-japanese-edges.py),
// each line's words, the costs of the words next to each other beyond the
// connection matrix, the cheapest path, and the cheapest path that writes
// the meant text (none when no path does: a dictionary gap). gen.tsv: id,
// reading, meant text. Lines out: id, reading, meant, found (0/1), the
// cheapest path's cost, the meant path's cost, words (begin,end,left,right,
// cost,known,text split by U+001F), joins (left,right,cost by spaces), the
// cheapest path and the meant one (word numbers by spaces). Only words on
// a path within `margin` of the cheapest are kept (--dump-margin, 5000),
// and the meant path's.
int RunDumpLattices(const ConnectionMatrix& matrix, const JapaneseConverter& converter, const std::filesystem::path& lines,
                    const std::filesystem::path& outPath, std::size_t shard, std::size_t shards, std::int64_t margin) {
    std::ifstream in(lines, std::ios::binary);
    std::ofstream out(outPath, std::ios::binary);
    if (!in || !out) {
        std::cerr << "could not open " << lines.string() << " or " << outPath.string() << "\n";
        return 1;
    }
    constexpr std::int64_t kInfinite = std::numeric_limits<std::int64_t>::max() / 4;
    std::size_t count = 0, found = 0, cheapest = 0, keptWords = 0;
    std::string line;
    for (std::size_t number = 0; std::getline(in, line); ++number) {
        if (number % shards != shard) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        const auto tab2 = tab == std::string::npos ? std::string::npos : line.find('\t', tab + 1);
        if (tab2 == std::string::npos) continue;
        const std::string id = line.substr(0, tab);
        const std::wstring reading = Widen(std::string_view(line).substr(tab + 1, tab2 - tab - 1));
        std::string meantField = line.substr(tab2 + 1);
        if (const auto more = meantField.find('\t'); more != std::string::npos) meantField.resize(more);
        // Full-width letters, digits and marks as half-width on both sides
        // (２つ目 meant, 2つ目 written): the same text.
        const auto fold = [](std::wstring text) {
            for (auto& c : text) {
                if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
                else if (c == 0x3000) c = L' ';
            }
            return text;
        };
        const std::wstring meantText = Widen(meantField);
        const std::wstring meant = fold(meantText);
        if (reading.empty() || meant.empty()) continue;
        const auto lattice = converter.ExportLattice(reading);
        const auto& words = lattice.words;
        std::vector<std::wstring> folded;
        folded.reserve(words.size());
        for (const auto& word : words) folded.push_back(fold(word.text));
        // What joins cost beyond the matrix, by the pair.
        std::map<std::pair<int, int>, std::int64_t> joins;
        for (const auto& join : lattice.joins) joins[{join.left, join.right}] = join.cost;
        const auto link = [&](int left, int right) -> std::int64_t {
            const auto extra = joins.find({left, right});
            return matrix.Cost(words[left].right, words[right].left) + (extra == joins.end() ? 0 : extra->second);
        };
        // The cheapest path through words whose texts write the meant text
        // in order: by word and the place in the meant text it starts at.
        using State = std::pair<int, std::size_t>;
        std::vector<std::vector<int>> starting(reading.size() + 1);
        for (int k = 0; k < static_cast<int>(words.size()); ++k) starting[words[k].begin].push_back(k);
        std::map<State, std::pair<std::int64_t, State>> reached;
        std::vector<std::vector<State>> endingAt(reading.size() + 1);
        for (const int k : starting[0]) {
            const auto& text = folded[k];
            if (text.empty() || meant.compare(0, text.size(), text) != 0) continue;
            reached[{k, 0}] = {words[k].cost + matrix.Cost(0, words[k].left), {-1, 0}};
            endingAt[words[k].end].push_back({k, 0});
        }
        for (std::size_t i = 1; i < reading.size(); ++i) {
            for (const auto& state : endingAt[i]) {
                const auto [p, from] = state;
                const std::int64_t before = reached[state].first;
                const std::size_t at = from + folded[p].size();
                if (at >= meant.size()) continue;
                for (const int k : starting[i]) {
                    const auto& text = folded[k];
                    if (text.empty() || meant.compare(at, text.size(), text) != 0) continue;
                    const std::int64_t cost = before + link(p, k) + words[k].cost;
                    auto [it, added] = reached.try_emplace(State{k, at}, kInfinite, State{-1, 0});
                    if (cost < it->second.first) it->second = {cost, state};
                    if (added) endingAt[words[k].end].push_back({k, at});
                }
            }
        }
        std::int64_t goldCost = kInfinite;
        State goldEnd{-1, 0};
        for (const auto& state : endingAt[reading.size()]) {
            if (state.second + folded[state.first].size() != meant.size()) continue;
            const std::int64_t cost = reached[state].first + matrix.Cost(words[state.first].right, 0);
            if (cost < goldCost) {
                goldCost = cost;
                goldEnd = state;
            }
        }
        std::vector<int> gold;
        for (State at = goldEnd; at.first >= 0; at = reached[at].second) gold.push_back(at.first);
        std::reverse(gold.begin(), gold.end());
        // The cheapest path through each word: to its end, and from there on.
        const std::size_t n = reading.size();
        std::vector<std::vector<int>> ending(n + 1);
        for (int k = 0; k < static_cast<int>(words.size()); ++k) ending[words[k].end].push_back(k);
        std::vector<std::int64_t> to(words.size(), kInfinite), from(words.size(), kInfinite);
        for (std::size_t i = 0; i < n; ++i) {
            for (const int k : starting[i]) {
                if (i == 0) {
                    to[k] = words[k].cost + matrix.Cost(0, words[k].left);
                    continue;
                }
                for (const int q : ending[i]) {
                    if (to[q] < kInfinite) to[k] = std::min(to[k], to[q] + link(q, k) + words[k].cost);
                }
            }
        }
        for (std::size_t i = n + 1; i-- > 1;) {
            for (const int k : ending[i]) {
                if (i == n) {
                    from[k] = matrix.Cost(words[k].right, 0);
                    continue;
                }
                for (const int r : starting[i]) {
                    if (from[r] < kInfinite) from[k] = std::min(from[k], link(k, r) + words[r].cost + from[r]);
                }
            }
        }
        std::vector<int> renumbered(words.size(), -1);
        std::vector<int> kept;
        std::vector<bool> onGold(words.size(), false);
        for (const int k : gold) onGold[k] = true;
        for (const int k : lattice.best) onGold[k] = true;
        for (int k = 0; k < static_cast<int>(words.size()); ++k) {
            const bool withinMargin = to[k] < kInfinite && from[k] < kInfinite && to[k] + from[k] <= lattice.bestCost + margin;
            if (withinMargin || onGold[k]) {
                renumbered[k] = static_cast<int>(kept.size());
                kept.push_back(k);
            }
        }
        ++count;
        if (!gold.empty()) ++found;
        if (!gold.empty() && gold == lattice.best) ++cheapest;
        keptWords += kept.size();
        out << id << '\t' << Narrow(reading) << '\t' << Narrow(meantText) << '\t' << (gold.empty() ? 0 : 1) << '\t'
            << lattice.bestCost << '\t' << (gold.empty() ? 0 : goldCost) << '\t';
        for (std::size_t j = 0; j < kept.size(); ++j) {
            const auto& w = words[kept[j]];
            if (j) out << '\x1f';
            out << w.begin << ',' << w.end << ',' << w.left << ',' << w.right << ',' << w.cost << ',' << (w.known ? 1 : 0)
                << ',' << Narrow(w.text);
        }
        out << '\t';
        bool first = true;
        for (const auto& join : lattice.joins) {
            if (renumbered[join.left] < 0 || renumbered[join.right] < 0) continue;
            out << (first ? "" : " ") << renumbered[join.left] << ',' << renumbered[join.right] << ',' << join.cost;
            first = false;
        }
        out << '\t';
        for (std::size_t j = 0; j < lattice.best.size(); ++j) out << (j ? " " : "") << renumbered[lattice.best[j]];
        out << '\t';
        for (std::size_t j = 0; j < gold.size(); ++j) out << (j ? " " : "") << renumbered[gold[j]];
        out << '\n';
    }
    std::cout << "lattices=" << count << " meant_path=" << 100.0 * found / std::max<std::size_t>(count, 1)
              << "% meant_is_cheapest=" << 100.0 * cheapest / std::max<std::size_t>(count, 1)
              << "% words_kept=" << static_cast<double>(keptWords) / std::max<std::size_t>(count, 1) << "\n";
    return 0;
}

// --edge-check <dir>: the C++ edge scorer against the scores the script
// wrote for a few lines (<dir>/test.tsv, export --data): the largest
// difference.
int RunEdgeCheck(const std::filesystem::path& dir) {
    tekito::japanese::EdgeScorer scorer;
    if (!scorer.Open(dir)) {
        std::cerr << "could not open the edge scorer at " << dir.string() << "\n";
        return 1;
    }
    std::ifstream in(dir / "test.tsv", std::ios::binary);
    std::string line;
    double largest = 0;
    std::size_t count = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
        if (fields.size() < 4) continue;
        std::vector<std::wstring> texts;
        std::vector<tekito::japanese::EdgeScorer::Word> words;
        std::vector<double> expected;
        texts.reserve(fields.size());
        for (std::size_t i = 3; i < fields.size(); ++i) {
            std::stringstream word(fields[i]);
            std::string b, e, l, r, c, k, v, t;
            std::getline(word, b, ',');
            std::getline(word, e, ',');
            std::getline(word, l, ',');
            std::getline(word, r, ',');
            std::getline(word, c, ',');
            std::getline(word, k, ',');
            std::getline(word, v, ',');
            std::getline(word, t);
            texts.push_back(Widen(t));
            words.push_back({static_cast<std::uint32_t>(std::stoul(b)), static_cast<std::uint32_t>(std::stoul(e)),
                             static_cast<std::uint16_t>(std::stoul(l)), static_cast<std::uint16_t>(std::stoul(r)),
                             std::stoll(c), k == "1", {}});
            expected.push_back(std::stod(v));
        }
        for (std::size_t i = 0; i < words.size(); ++i) words[i].text = texts[i];
        const auto scores = scorer.Score(Widen(fields[0]), Widen(fields[1]), Widen(fields[2]), words);
        for (std::size_t i = 0; i < scores.size(); ++i) {
            largest = std::max(largest, std::abs(scores[i] - expected[i]));
            ++count;
        }
    }
    std::cout << "words=" << count << " largest_difference=" << largest << " cost_scale=1/" << 1.0 / scorer.CostScale()
              << "\n";
    // The linear layers run in int8: a few hundredths of a nat apart.
    return largest < 0.1 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path pack, wikimedia, corpus, keysCorpus, romaji, dumpPhrases, writeSentences, guessReadings, guessReadingsOut;
    bool byPhrase = false;
    bool useContext = true;
    std::filesystem::path modelPack;
    std::filesystem::path rerankerPack;
    std::filesystem::path slipsPack;
    JapaneseConverter::ModelWeights weights;
    std::filesystem::path topicPack;
    // --english-ties <data folder>: english-pairs and japanese-english-senses there.
    std::filesystem::path englishTiesRoot;
    std::filesystem::path collocationsPack;
    std::filesystem::path homophonesPack;
    std::size_t showMisses = 0;
    TypingStudyOptions study;
    ChunkOptions chunks;
    std::filesystem::path chunkCorpus;
    std::filesystem::path dumpLattices, dumpOut;
    std::filesystem::path edgeCheck;
    std::size_t dumpShard = 0, dumpShards = 1;
    std::int64_t dumpMargin = 5000;
    // English for katakana candidates: the japanese-loanwords and
    // japanese-english packs.
    std::filesystem::path loanwordPack, japaneseEnglishPack;
    tekito::japanese::KeyConverter::Costs costs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == "--wikimedia" && i + 1 < argc) wikimedia = argv[++i];
        else if (arg == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (arg == "--keys-corpus" && i + 1 < argc) keysCorpus = argv[++i];
        else if (arg == "--romaji" && i + 1 < argc) romaji = argv[++i];
        else if (arg == "--typo" && i + 1 < argc) costs.typo = std::stoll(argv[++i]);
        else if (arg == "--typo-early" && i + 1 < argc) costs.earlyPercent = std::stoll(argv[++i]);
        else if (arg == "--typo-late" && i + 1 < argc) costs.latePercent = std::stoll(argv[++i]);
        else if (arg == "--typo-replaced" && i + 1 < argc) costs.replacedPercent = std::stoll(argv[++i]);
        else if (arg == "--typo-dropped" && i + 1 < argc) costs.droppedPercent = std::stoll(argv[++i]);
        else if (arg == "--typo-added" && i + 1 < argc) costs.addedPercent = std::stoll(argv[++i]);
        else if (arg == "--typo-swapped" && i + 1 < argc) costs.swappedPercent = std::stoll(argv[++i]);
        else if (arg == "--typo-repeated" && i + 1 < argc) costs.repeatedPercent = std::stoll(argv[++i]);
        else if (arg == "--typo-lattice") costs.contextDecides = false;
        else if (arg == "--typo-vowels-only") costs.consonantsDropped = false;
        else if (arg == "--typo-no-vowel-swaps") costs.vowelsReplaced = false;
        else if (arg == "--typo-keep" && i + 1 < argc) costs.keepTypedBy = std::stoll(argv[++i]);
        else if (arg == "--dump-phrases" && i + 1 < argc) dumpPhrases = argv[++i];
        else if (arg == "--write-sentences" && i + 1 < argc) writeSentences = argv[++i];
        else if (arg == "--guess-readings" && i + 2 < argc) {
            guessReadings = argv[++i];
            guessReadingsOut = argv[++i];
        }
        else if (arg == "--by-phrase") byPhrase = true;
        else if (arg == "--lm" && i + 1 < argc) modelPack = argv[++i];
        else if (arg == "--reranker" && i + 1 < argc) rerankerPack = argv[++i];
        else if (arg == "--context-previous") gContextPrevious = true;
        else if (arg == "--added" && i + 1 < argc) gAddedPacks.emplace_back(argv[++i]);
        else if (arg == "--context" && i + 1 < argc) {
            std::ifstream in(argv[++i], std::ios::binary);
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto tab = line.find('\t');
                if (tab != std::string::npos) gContexts[line.substr(0, tab)] = ja_eval::Widen(line.substr(tab + 1));
            }
        }
        else if (arg == "--ai" && i + 1 < argc && std::string_view(argv[i + 1]) == "probe") {
            ++i;
            gAiJudge = &gProbe;
        }
        else if (arg == "--ai" && i + 1 < argc) {
            const std::string_view which = argv[++i];
            const auto model = which == "strong" ? tekito::japanese::AiModel::Strong : tekito::japanese::AiModel::Light;
            // TEKITO_AI_DEVICE=cpu|gpu|npu (else auto).
            const char* device = std::getenv("TEKITO_AI_DEVICE");
            gAiJudge = tekito::japanese::ProcessAiJudge(
                model, device && std::string_view(device) == "cpu"   ? tekito::japanese::AiDevice::Cpu
                       : device && std::string_view(device) == "gpu" ? tekito::japanese::AiDevice::Gpu
                       : device && std::string_view(device) == "npu" ? tekito::japanese::AiDevice::Npu
                                                                      : tekito::japanese::AiDevice::Auto);
            if (!gAiJudge) {
                std::cerr << "the " << which << " AI model is not installed\n";
                return 1;
            }
            // TEKITO_AI_WAIT_MS=N: how long a conversion waits for the answer
            // (an evaluation on the processor while the GPU trains).
            if (const char* wait = std::getenv("TEKITO_AI_WAIT_MS")) {
                static const tekito::japanese::AiHelperClient patient(
                    model,
                    device && std::string_view(device) == "cpu"   ? tekito::japanese::AiDevice::Cpu
                    : device && std::string_view(device) == "gpu" ? tekito::japanese::AiDevice::Gpu
                                                                  : tekito::japanese::AiDevice::Auto,
                    std::chrono::milliseconds(std::atoi(wait)), gAiJudge->Top());
                gAiJudge = &patient;
            }
        }
        else if (arg == "--slips" && i + 1 < argc) slipsPack = argv[++i];
        else if (arg == "--lm-scale" && i + 1 < argc) weights.costPerNat = std::stoll(argv[++i]);
        else if (arg == "--lm-penalty" && i + 1 < argc) weights.penaltyPerNat = std::stoll(argv[++i]);
        else if (arg == "--lm-threshold" && i + 1 < argc) weights.threshold = std::stod(argv[++i]);
        else if (arg == "--topic-scale" && i + 1 < argc) weights.costPerTopic = std::stoll(argv[++i]);
        else if (arg == "--topic-vectors" && i + 1 < argc) topicPack = argv[++i];
        else if (arg == "--english-ties" && i + 1 < argc) englishTiesRoot = argv[++i];
        else if (arg == "--collocations" && i + 1 < argc) collocationsPack = argv[++i];
        else if (arg == "--homophones" && i + 1 < argc) homophonesPack = argv[++i];
        else if (arg == "--homophone-scale" && i + 1 < argc) weights.costPerHomophone = std::stoll(argv[++i]);
        else if (arg == "--colloc-scale" && i + 1 < argc) weights.costPerCollocation = std::stoll(argv[++i]);
        else if (arg == "--colloc-ceiling" && i + 1 < argc) weights.collocationCeiling = std::stod(argv[++i]);
        else if (arg == "--english-scale" && i + 1 < argc) weights.costPerEnglish = std::stoll(argv[++i]);
        else if (arg == "--english-ja-scale" && i + 1 < argc) weights.costPerEnglishJapanese = std::stoll(argv[++i]);
        else if (arg == "--topic-vector-scale" && i + 1 < argc) weights.costPerVector = std::stoll(argv[++i]);
        else if (arg == "--no-context") useContext = false;
        else if (arg == "--show-misses" && i + 1 < argc) showMisses = std::stoul(argv[++i]);
        else if (arg == "--study" && i + 1 < argc) study.study = argv[++i];
        else if (arg == "--join" && i + 1 < argc) study.join = std::stoul(argv[++i]);
        else if (arg == "--sample" && i + 1 < argc) study.sample = std::stoul(argv[++i]);
        else if (arg == "--chunks" && i + 1 < argc) chunkCorpus = argv[++i];
        else if (arg == "--loanwords" && i + 1 < argc) loanwordPack = argv[++i];
        else if (arg == "--japanese-english" && i + 1 < argc) japaneseEnglishPack = argv[++i];
        else if (arg == "--english-scores" && i + 1 < argc) chunks.englishScores = argv[++i];
        else if (arg == "--english-score" && i + 1 < argc) chunks.thresholds.commonWord = std::stod(argv[++i]);
        else if (arg == "--japanese-cost" && i + 1 < argc) chunks.thresholds.costPerKana = std::stoll(argv[++i]);
        else if (arg == "--known-name" && i + 1 < argc) chunks.thresholds.knownName = std::stod(argv[++i]);
        else if (arg == "--show-chunks" && i + 1 < argc) chunks.showChunks = std::stoul(argv[++i]);
        else if (arg == "--japanese-mode") chunks.japaneseMode = true;
        else if (arg == "--no-slips") chunks.slips = false;
        else if (arg == "--english-engine") chunks.englishEngine = true;
        else if (arg == "--no-run-together") chunks.runTogether = false;
        else if (arg == "--sentence-judge") chunks.sentenceJudge = true;
        else if (arg == "--hold") chunks.hold = true;
        else if (arg == "--write-chunks" && i + 1 < argc) chunks.writeChunks = argv[++i];
        else if (arg == "--dump-lattices" && i + 1 < argc) dumpLattices = argv[++i];
        else if (arg == "--edge-check" && i + 1 < argc) edgeCheck = argv[++i];
        else if (arg == "--dump-out" && i + 1 < argc) dumpOut = argv[++i];
        else if (arg == "--dump-margin" && i + 1 < argc) dumpMargin = std::stoll(argv[++i]);
        else if (arg == "--dump-shard" && i + 1 < argc) {
            const std::string value = argv[++i];
            const auto slash = value.find('/');
            dumpShard = std::stoul(value.substr(0, slash));
            dumpShards = slash == std::string::npos ? 1 : std::stoul(value.substr(slash + 1));
        }
    }
    tekito::japanese::LanguageModel model;
    if (!modelPack.empty() && !model.Open(modelPack)) {
        std::cerr << "could not open the language model at " << modelPack.string() << "\n";
        return 1;
    }
    tekito::japanese::TopicVectors topicVectors;
    tekito::japanese::Homophones homophones;
    if (!homophonesPack.empty() && !homophones.Open(homophonesPack)) {
        std::cerr << "could not open the homophones at " << homophonesPack.string() << "\n";
        return 2;
    }
    tekito::japanese::Collocations collocations;
    if (!collocationsPack.empty() && !collocations.Open(collocationsPack)) {
        std::cerr << "could not open the collocations at " << collocationsPack.string() << "\n";
        return 2;
    }
    tekito::japanese::EnglishTies englishTies;
    if (!englishTiesRoot.empty() &&
        !englishTies.Open(englishTiesRoot / L"english-pairs", englishTiesRoot / L"japanese-english-senses")) {
        std::cerr << "could not open the English ties in " << englishTiesRoot.string() << "\n";
        return 2;
    }
    if (!topicPack.empty() && !topicVectors.Open(topicPack)) {
        std::cerr << "could not open the topic vectors at " << topicPack.string() << "\n";
        return 1;
    }
    if (!edgeCheck.empty()) return RunEdgeCheck(edgeCheck);
    if (!pack.empty() && !dumpLattices.empty()) {
        JapaneseDictionary dictionary;
        ConnectionMatrix matrix;
        if (!dictionary.Open(pack / "dictionary.bin") || !matrix.Open(pack / "connection.bin")) {
            std::cerr << "could not open the japanese-core pack at " << pack.string() << "\n";
            return 1;
        }
        if (!wikimedia.empty()) (void)dictionary.AddWords(wikimedia / "dictionary.bin");
        for (const auto& more : gAddedPacks) (void)dictionary.AddWords(more / "dictionary.bin");
        JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
        SetNumberWord(converter, pack);
        return RunDumpLattices(matrix, converter, dumpLattices, dumpOut, dumpShard, dumpShards, dumpMargin);
    }
    if (!pack.empty() && !keysCorpus.empty() && !chunkCorpus.empty()) keysCorpus.clear();
    if (!pack.empty() && !chunkCorpus.empty()) {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
#endif
        JapaneseDictionary dictionary;
        ConnectionMatrix matrix;
        if (!dictionary.Open(pack / "dictionary.bin") || !matrix.Open(pack / "connection.bin")) {
            std::cerr << "could not open the japanese-core pack at " << pack.string() << "\n";
            return 1;
        }
        if (!wikimedia.empty()) (void)dictionary.AddWords(wikimedia / "dictionary.bin");
        for (const auto& more : gAddedPacks) (void)dictionary.AddWords(more / "dictionary.bin");
        JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
        if (topicVectors.IsOpen()) converter.SetTopicVectors(&topicVectors);
        if (englishTies.IsOpen()) converter.SetEnglishTies(&englishTies);
        if (collocations.IsOpen()) converter.SetCollocations(&collocations);
        if (homophones.IsOpen()) converter.SetHomophones(&homophones);
        SetNumberWord(converter, pack);
        chunks.pack = pack;
        chunks.rerankerPack = rerankerPack;
        chunks.slipsPack = slipsPack;
        tekito::japanese::Loanwords names;
        if (!loanwordPack.empty()) (void)names.Open(loanwordPack);
        if (!japaneseEnglishPack.empty()) (void)names.OpenEnglish(japaneseEnglishPack);
        return RunChunks(matrix, dictionary, converter, chunkCorpus, romaji, costs, showMisses, chunks,
                         names.IsOpen() ? &names : nullptr);
    }
    // --guess-readings IN OUT: each sentence of IN (one per line) with the
    // reading the dictionary gives it, as a corpus (source, id, reading,
    // text), for making whole-sentence lists to train a reranker on.
    if (!pack.empty() && !guessReadings.empty()) {
        JapaneseDictionary dictionary;
        ConnectionMatrix matrix;
        if (!dictionary.Open(pack / "dictionary.bin") || !matrix.Open(pack / "connection.bin")) {
            std::cerr << "could not open the japanese-core pack at " << pack.string() << "\n";
            return 1;
        }
        if (!wikimedia.empty()) (void)dictionary.AddWords(wikimedia / "dictionary.bin");
        for (const auto& more : gAddedPacks) (void)dictionary.AddWords(more / "dictionary.bin");
        const JapaneseConverter converter(dictionary, matrix);
        std::ifstream in(guessReadings, std::ios::binary);
        std::ofstream out(guessReadingsOut, std::ios::binary);
        std::string line;
        std::size_t read = 0, written = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ++read;
            const auto guess = converter.ReadingOf(ja_eval::Widen(line));
            if (!guess || guess->reading.empty()) continue;
            out << "train\t" << read << "\t" << Narrow(guess->reading) << "\t" << line << "\n";
            ++written;
        }
        std::cout << "read " << read << ", wrote " << written << "\n";
        return 0;
    }
    if (!pack.empty() && !keysCorpus.empty()) {
#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
#endif
        JapaneseDictionary dictionary;
        ConnectionMatrix matrix;
        if (!dictionary.Open(pack / "dictionary.bin") || !matrix.Open(pack / "connection.bin")) {
            std::cerr << "could not open the japanese-core pack at " << pack.string() << "\n";
            return 1;
        }
        if (!wikimedia.empty()) (void)dictionary.AddWords(wikimedia / "dictionary.bin");
        for (const auto& more : gAddedPacks) (void)dictionary.AddWords(more / "dictionary.bin");
        JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
        if (topicVectors.IsOpen()) converter.SetTopicVectors(&topicVectors);
        if (englishTies.IsOpen()) converter.SetEnglishTies(&englishTies);
        if (collocations.IsOpen()) converter.SetCollocations(&collocations);
        if (homophones.IsOpen()) converter.SetHomophones(&homophones);
        SetNumberWord(converter, pack);
        if (!study.study.empty()) {
            study.pack = pack;
            study.showExamples = showMisses;
            return RunTypingStudy(dictionary, converter, keysCorpus, romaji, study);
        }
        tekito::japanese::Loanwords loanwords;
        if (!loanwordPack.empty()) (void)loanwords.Open(loanwordPack);
        if (!japaneseEnglishPack.empty()) (void)loanwords.OpenEnglish(japaneseEnglishPack);
        tekito::japanese::SentenceReranker reranker;
        if (!rerankerPack.empty() && !reranker.Open(rerankerPack)) {
            std::cerr << "could not open the reranker at " << rerankerPack.string() << "\n";
            return 1;
        }
        tekito::japanese::KanaSlips slips;
        if (!slipsPack.empty() && !slips.Open(slipsPack)) {
            std::cerr << "could not open the slip finder at " << slipsPack.string() << "\n";
            return 1;
        }
        return RunKeys(matrix, dictionary, converter, keysCorpus, romaji, costs, showMisses, dumpPhrases, writeSentences, byPhrase,
                       useContext, loanwords.IsOpen() ? &loanwords : nullptr, reranker.IsOpen() ? &reranker : nullptr,
                       slips.IsOpen() ? &slips : nullptr);
    }
    if (pack.empty() || corpus.empty()) {
        std::cerr << "usage: tekito_ja_eval --pack <dir> --corpus <japanese_eval.tsv> [--show-misses N]\n";
        return 2;
    }
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    JapaneseDictionary dictionary;
    ConnectionMatrix matrix;
    if (!dictionary.Open(pack / "dictionary.bin") || !matrix.Open(pack / "connection.bin")) {
        std::cerr << "could not open the japanese-core pack at " << pack.string() << "\n";
        return 1;
    }
    if (!wikimedia.empty()) (void)dictionary.AddWords(wikimedia / "dictionary.bin");
    for (const auto& more : gAddedPacks) (void)dictionary.AddWords(more / "dictionary.bin");
    JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
        if (topicVectors.IsOpen()) converter.SetTopicVectors(&topicVectors);
        if (englishTies.IsOpen()) converter.SetEnglishTies(&englishTies);
        if (collocations.IsOpen()) converter.SetCollocations(&collocations);
        if (homophones.IsOpen()) converter.SetHomophones(&homophones);
        SetNumberWord(converter, pack);
    const auto rows = LoadCorpus(corpus);
    if (rows.empty()) {
        std::cerr << "no rows in " << corpus.string() << "\n";
        return 1;
    }

    std::ofstream readingSentences;
    if (!writeSentences.empty()) readingSentences.open(writeSentences, std::ios::binary);
    std::map<std::string, Totals> totals;
    std::size_t shown = 0;
    for (const auto& row : rows) {
        const auto start = Clock::now();
        const auto phrases = converter.Convert(row.reading);
        const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        std::wstring best;
        for (const auto& phrase : phrases) {
            if (!phrase.candidates.empty()) best += phrase.candidates.front().text;
        }
        std::size_t distance = SIZE_MAX;
        std::size_t length = 0;
        for (const auto& expected : row.expected) {
            const std::size_t d = EditDistance(best, expected);
            if (d < distance) {
                distance = d;
                length = expected.size();
            }
        }
        if (readingSentences.is_open()) {
            // Whole sentences from the phrases' lists, as with --keys-corpus.
            std::vector<std::vector<std::wstring>> lists;
            for (const auto& phrase : phrases) {
                lists.emplace_back();
                for (const auto& candidate : phrase.candidates) {
                    if (lists.back().size() == 20) break;
                    if (std::find(lists.back().begin(), lists.back().end(), candidate.text) == lists.back().end()) {
                        lists.back().push_back(candidate.text);
                    }
                }
            }
            const auto sentences = SentencesByRank(lists, 50);
            for (std::size_t s = 0; s < sentences.size(); ++s) {
                const bool meant = std::find(row.expected.begin(), row.expected.end(), sentences[s]) != row.expected.end();
                readingSentences << row.source << "\t" << row.id << "\t" << s << "\t" << (meant ? 1 : 0) << "\t"
                                 << Narrow(sentences[s]) << "\n";
            }
        }
        for (Totals* t : {&totals[row.source], &totals["all"]}) {
            ++t->rows;
            if (distance == 0) ++t->exact;
            if (distance == 0 || ReachableWithin(phrases, row.expected, 10)) ++t->top10;
            t->errors += distance;
            t->characters += length;
            t->microseconds.push_back(elapsed);
        }
        if (distance != 0 && shown < showMisses) {
            ++shown;
            std::cout << "miss " << row.source << ":" << row.id << "  " << Narrow(best) << "  (want "
                      << Narrow(row.expected.front()) << ")\n";
        }
    }

    std::cout << std::fixed << std::setprecision(1);
    for (const auto& [source, t] : totals) {
        std::cout << source << ": rows=" << t.rows << " top1=" << 100.0 * t.exact / t.rows << "%"
                  << " top10_per_phrase=" << 100.0 * t.top10 / t.rows << "%"
                  << " cer=" << (t.characters ? 100.0 * t.errors / t.characters : 0.0) << "%"
                  << " convert_us_p50=" << Percentile(t.microseconds, 0.5)
                  << " p95=" << Percentile(t.microseconds, 0.95)
                  << " max=" << Percentile(t.microseconds, 1.0) << "\n";
    }
    return 0;
}
