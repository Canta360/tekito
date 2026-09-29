// Offline accuracy of Japanese conversion: converts each reading of
// eval/generated/japanese_eval.tsv (scripts/prepare-japanese-eval.py) with
// the japanese-core pack and compares the first conversion with the
// acceptable ones.
//
// tekito_ja_eval --pack <data/japanese-core> --corpus <japanese_eval.tsv> [--show-misses N]
// tekito_ja_eval --pack <data/japanese-core> --keys-corpus <japanese_eval_keys.tsv>
//                --romaji <data/japanese-romaji> [--typo N] [--dump-phrases <path>]
//                [--by-phrase [--no-context]]
//                [--lm <data/japanese-lm> [--lm-scale N] [--lm-penalty N] [--lm-threshold X] [--topic-scale N]]
// tekito_ja_eval --pack <data/japanese-core> --keys-corpus <keys.tsv> --romaji <data/japanese-romaji>
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
// Per source: sentences whose first conversion is acceptable, the character
// error rate against the closest acceptable text, how often an acceptable
// text is among the first phrase-by-phrase choices, and conversion time.

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/RomajiTable.h"
#include "JapaneseEvalCommon.h"
#include "JapaneseTypingStudy.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

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
            const std::filesystem::path& dumpPhrases, bool byPhrase, bool useContext) {
    const auto table = tekito::japanese::RomajiTable::Load(romaji);
    if (!table) {
        std::cerr << "could not open the romaji table\n";
        return 1;
    }
    const tekito::japanese::KeyConverter keyConverter(dictionary, matrix, converter, *table, costs);
    tekito::japanese::JapaneseComposer composer(table.get());
    composer.SetConverter(&converter);
    composer.SetKeyConverter(&keyConverter);

    struct KeyTotals {
        std::size_t rows{0}, exact{0}, inList{0}, errors{0}, characters{0};
        std::vector<double> microseconds;
        std::vector<double> listMicroseconds;
    };
    std::map<std::string, KeyTotals> totals;
    std::size_t shown = 0;
    // --dump-phrases: the phrases of sentences that convert right, one per
    // row, as a keys file (for typing phrase by phrase).
    std::ofstream phrases;
    if (!dumpPhrases.empty()) phrases.open(dumpPhrases, std::ios::binary);
    // Keys files share the layout source, id, keys, expected...: LoadCorpus
    // reads keys where it reads readings.
    for (const auto& row : LoadCorpus(corpus)) {
        composer.Clear();
        for (const wchar_t key : row.reading) composer.Insert(key);
        const auto start = Clock::now();
        composer.Convert();
        const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        std::wstring output = composer.Preedit();
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
        for (KeyTotals* t : {&totals[row.source], &totals["all"]}) {
            ++t->rows;
            if (distance == 0) ++t->exact;
            if (inList) ++t->inList;
            t->listMicroseconds.push_back(listElapsed);
            t->errors += distance;
            t->characters += length;
            t->microseconds.push_back(elapsed);
        }
        if (distance != 0 && shown < showMisses) {
            ++shown;
            std::cout << "miss " << row.source << ":" << row.id << "  " << Narrow(output) << "  (want "
                      << Narrow(row.expected.front()) << (inList ? ", in the list" : "") << ")\n";
        }
    }
    std::cout << std::fixed << std::setprecision(1);
    for (const auto& [source, t] : totals) {
        std::cout << source << ": rows=" << t.rows << " top1=" << 100.0 * t.exact / t.rows << "%"
                  << " in_list=" << 100.0 * t.inList / t.rows << "%"
                  << " cer=" << (t.characters ? 100.0 * t.errors / t.characters : 0.0) << "%"
                  << " convert_us_p50=" << Percentile(t.microseconds, 0.5)
                  << " p95=" << Percentile(t.microseconds, 0.95)
                  << " max=" << Percentile(t.microseconds, 1.0)
                  << " list_us_p50=" << Percentile(t.listMicroseconds, 0.5)
                  << " p95=" << Percentile(t.listMicroseconds, 0.95)
                  << " max=" << Percentile(t.listMicroseconds, 1.0) << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path pack, corpus, keysCorpus, romaji, dumpPhrases;
    bool byPhrase = false;
    bool useContext = true;
    std::filesystem::path modelPack;
    JapaneseConverter::ModelWeights weights;
    std::size_t showMisses = 0;
    TypingStudyOptions study;
    tekito::japanese::KeyConverter::Costs costs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (arg == "--keys-corpus" && i + 1 < argc) keysCorpus = argv[++i];
        else if (arg == "--romaji" && i + 1 < argc) romaji = argv[++i];
        else if (arg == "--typo" && i + 1 < argc) costs.typo = std::stoll(argv[++i]);
        else if (arg == "--dump-phrases" && i + 1 < argc) dumpPhrases = argv[++i];
        else if (arg == "--by-phrase") byPhrase = true;
        else if (arg == "--lm" && i + 1 < argc) modelPack = argv[++i];
        else if (arg == "--lm-scale" && i + 1 < argc) weights.costPerNat = std::stoll(argv[++i]);
        else if (arg == "--lm-penalty" && i + 1 < argc) weights.penaltyPerNat = std::stoll(argv[++i]);
        else if (arg == "--lm-threshold" && i + 1 < argc) weights.threshold = std::stod(argv[++i]);
        else if (arg == "--topic-scale" && i + 1 < argc) weights.costPerTopic = std::stoll(argv[++i]);
        else if (arg == "--no-context") useContext = false;
        else if (arg == "--show-misses" && i + 1 < argc) showMisses = std::stoul(argv[++i]);
        else if (arg == "--study" && i + 1 < argc) study.study = argv[++i];
        else if (arg == "--join" && i + 1 < argc) study.join = std::stoul(argv[++i]);
        else if (arg == "--sample" && i + 1 < argc) study.sample = std::stoul(argv[++i]);
    }
    tekito::japanese::LanguageModel model;
    if (!modelPack.empty() && !model.Open(modelPack)) {
        std::cerr << "could not open the language model at " << modelPack.string() << "\n";
        return 1;
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
        JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
        if (!study.study.empty()) {
            study.pack = pack;
            study.showExamples = showMisses;
            return RunTypingStudy(dictionary, converter, keysCorpus, romaji, study);
        }
        return RunKeys(matrix, dictionary, converter, keysCorpus, romaji, costs, showMisses, dumpPhrases, byPhrase,
                       useContext);
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
    JapaneseConverter converter(dictionary, matrix);
        if (model.IsOpen()) converter.SetLanguageModel(&model, weights);
    const auto rows = LoadCorpus(corpus);
    if (rows.empty()) {
        std::cerr << "no rows in " << corpus.string() << "\n";
        return 1;
    }

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
