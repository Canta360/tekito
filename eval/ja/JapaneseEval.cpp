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
// --wikipedia <data/ja/japanese-wikipedia> adds the words of Wikipedia
// article titles to the dictionary, as TEKITO does when the pack is there.
//
// Per source: sentences whose first conversion is acceptable, the character
// error rate against the closest acceptable text, how often an acceptable
// text is among the first phrase-by-phrase choices, and conversion time.

#include "Core/ConversionEngine.h"
#include "Core/EnglishSpelling.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/Japanese/ChunkJudge.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/Japanese/KanaSlips.h"
#include "JapaneseEvalCommon.h"
#include "JapaneseTypingStudy.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// Digits and numbers read in kana as one number word, as the text service
// sets it up (JapaneseData: japanese-core/pos.tsv, "number").
void SetNumberWord(tekito::japanese::JapaneseConverter& converter, const std::filesystem::path& pack) {
    const auto parts = tekito::japanese::UserPartsOfSpeech::Load(pack / "pos.tsv");
    if (const auto number = parts.Number()) {
        converter.SetNumberWord(tekito::japanese::JapaneseConverter::NumberWord{number->left, number->right, number->cost});
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

struct Totals {
    std::size_t rows{0};
    std::size_t exact{0};
    std::size_t top10{0};
    std::size_t errors{0};
    std::size_t characters{0};
    std::vector<double> microseconds;
};

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
    composer.SetKeyConverter(&keyConverter);
    if (loanwords) composer.SetLoanwords(loanwords);
    composer.SetSentenceReranker(reranker);
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
    composer.SetLiveConversion(live);
    // TEKITO_EVAL_LIVE_OPEN=N: phrases kept open (0: none settled).
    if (const char* open = std::getenv("TEKITO_EVAL_LIVE_OPEN"); open && *open) {
        const auto count = std::stoul(open);
        composer.SetLiveOpenPhrases(count == 0 ? std::size_t{1} << 20 : count);
    }
    for (const auto& row : LoadCorpus(corpus)) {
        composer.Clear();
        // Each row on its own: Enter (live) commits, which would lead the
        // next row on from it.
        if (live) composer.ForgetContext();
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
        // Whole sentences from the phrases' lists, likeliest first: where
        // what was meant is among them (for choosing again with a model).
        std::size_t sentenceRank = SIZE_MAX;
        if (!byPhrase) {
            std::vector<std::vector<std::wstring>> lists;
            std::vector<std::wstring> wholes;
            for (std::size_t f = 0; f < phraseCount; ++f) {
                composer.MoveFocus(-static_cast<int>(phraseCount));
                composer.MoveFocus(static_cast<int>(f));
                composer.NextCandidate();
                composer.PreviousCandidate();
                const auto* candidates = composer.FocusedCandidates();
                lists.emplace_back();
                for (std::size_t j = 0; candidates && j < candidates->size() && lists.back().size() < 20; ++j) {
                    // A whole-input candidate is a sentence of its own.
                    if (!(*candidates)[j].reading.empty()) {
                        wholes.push_back((*candidates)[j].text);
                    } else if (std::find(lists.back().begin(), lists.back().end(), (*candidates)[j].text) ==
                               lists.back().end()) {
                        lists.back().push_back((*candidates)[j].text);
                    }
                }
            }
            auto sentences = SentencesByRank(lists, kSentences);
            for (auto& whole : wholes) {
                if (std::find(sentences.begin(), sentences.end(), whole) == sentences.end()) sentences.push_back(whole);
            }
            for (std::size_t s = 0; s < sentences.size() && sentenceRank == SIZE_MAX; ++s) {
                if (std::find(row.expected.begin(), row.expected.end(), sentences[s]) != row.expected.end()) sentenceRank = s;
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
    if (options.slips) composer.SetKeyConverter(&keyConverter);
    if (names) composer.SetLoanwords(names);
    tekito::japanese::SentenceReranker reranker;
    if (!options.rerankerPack.empty() && !reranker.Open(options.rerankerPack)) {
        std::cerr << "could not open the reranker at " << options.rerankerPack.string() << "\n";
        return 1;
    }
    if (reranker.IsOpen()) composer.SetSentenceReranker(&reranker);
    tekito::japanese::KanaSlips kanaSlips;
    if (!options.slipsPack.empty() && !kanaSlips.Open(options.slipsPack)) {
        std::cerr << "could not open the slip finder at " << options.slipsPack.string() << "\n";
        return 1;
    }
    if (kanaSlips.IsOpen()) composer.SetKanaSlips(&kanaSlips);
    const tekito::japanese::ChunkJudge judge(*table, converter, options.slips ? &keyConverter : nullptr, english, names,
                                             options.thresholds);
    const auto parts = tekito::japanese::UserPartsOfSpeech::Load(options.pack / "pos.tsv");
    const auto noun = parts.For(tekito::japanese::UserWordKind::ProperNoun);
    if (!options.japaneseMode) composer.SetMixedTyping(&judge, noun ? noun->right : 0);
    if (options.runTogether) composer.SetRunTogether(&judge);
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
    };
    std::map<std::string, ChunkTotals> totals;
    std::map<std::string, std::size_t> sentenceRows, sentenceRight;
    std::size_t shownMisses = 0, shownChunks = 0;
    for (const auto& row : LoadCorpus(corpus)) {
        std::wstring output;
        std::size_t englishBefore = 0;
        std::wstring englishText;
        bool japaneseInSentence = false;
        composer.ForgetContext();
        composer.SetSentenceStart(true);
        std::vector<std::pair<bool, bool>> judged;  // English meant, English said
        std::wistringstream words(row.reading);
        std::size_t chunkIndex = 0;
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
            for (const wchar_t key : chunk) composer.Insert(key);
            composer.Convert();
            if (options.hold) continue;
            const auto choices = written.is_open() ? OnePickAway(composer) : std::vector<std::wstring>{};
            const auto committed = composer.Commit();
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
                  << " cer=" << (t.characters ? 100.0 * t.errors / t.characters : 0.0) << "%"
                  << " english_chunks=" << t.english << " judged_english="
                  << (t.english ? 100.0 * t.englishRight / t.english : 0.0) << "%"
                  << " japanese_chunks=" << t.japanese << " judged_japanese="
                  << (t.japanese ? 100.0 * t.japaneseRight / t.japanese : 0.0) << "%\n";
    }
    for (const auto& [source, rows] : sentenceRows) {
        std::cout << source << ": sentence_judged_rows=" << 100.0 * sentenceRight[source] / rows << "%\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path pack, wikipedia, corpus, keysCorpus, romaji, dumpPhrases, writeSentences, guessReadings, guessReadingsOut;
    bool byPhrase = false;
    bool useContext = true;
    std::filesystem::path modelPack;
    std::filesystem::path rerankerPack;
    std::filesystem::path slipsPack;
    JapaneseConverter::ModelWeights weights;
    std::size_t showMisses = 0;
    TypingStudyOptions study;
    ChunkOptions chunks;
    std::filesystem::path chunkCorpus;
    // English for katakana candidates: the japanese-loanwords and
    // japanese-english packs.
    std::filesystem::path loanwordPack, japaneseEnglishPack;
    tekito::japanese::KeyConverter::Costs costs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == "--wikipedia" && i + 1 < argc) wikipedia = argv[++i];
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
        else if (arg == "--typo-lattice") costs.contextDecides = false;
        else if (arg == "--typo-vowels-only") costs.consonantsDropped = false;
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
        else if (arg == "--slips" && i + 1 < argc) slipsPack = argv[++i];
        else if (arg == "--lm-scale" && i + 1 < argc) weights.costPerNat = std::stoll(argv[++i]);
        else if (arg == "--lm-penalty" && i + 1 < argc) weights.penaltyPerNat = std::stoll(argv[++i]);
        else if (arg == "--lm-threshold" && i + 1 < argc) weights.threshold = std::stod(argv[++i]);
        else if (arg == "--topic-scale" && i + 1 < argc) weights.costPerTopic = std::stoll(argv[++i]);
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
    }
    tekito::japanese::LanguageModel model;
    if (!modelPack.empty() && !model.Open(modelPack)) {
        std::cerr << "could not open the language model at " << modelPack.string() << "\n";
        return 1;
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
        if (!wikipedia.empty()) (void)dictionary.AddWords(wikipedia / "dictionary.bin");
        JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
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
        if (!wikipedia.empty()) (void)dictionary.AddWords(wikipedia / "dictionary.bin");
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
        if (!wikipedia.empty()) (void)dictionary.AddWords(wikipedia / "dictionary.bin");
        JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
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
    if (!wikipedia.empty()) (void)dictionary.AddWords(wikipedia / "dictionary.bin");
    JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
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
