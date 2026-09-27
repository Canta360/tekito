// Offline accuracy of Japanese conversion: converts each reading of
// eval/generated/japanese_eval.tsv (scripts/prepare-japanese-eval.py) with
// the japanese-core pack and compares the first conversion with the
// acceptable ones.
//
// tekito_ja_eval --pack <data/japanese-core> --corpus <japanese_eval.tsv> [--show-misses N]
// tekito_ja_eval --pack <data/japanese-core> --keys-corpus <japanese_eval_keys.tsv>
//                --romaji <data/japanese-romaji> --english <data/japanese-english-words>
//                [--no-mixed] [--english-base N] [--english-per-score N] [--language-switch N] [--typo N]
//
// The second form types the romaji keys into the composer, as TEKITO does,
// and converts with Space; "english" counts outputs with Latin letters in
// them (wanted for the mixed sentences, a mistake for the others).
//
// Per source: sentences whose first conversion is acceptable, the character
// error rate against the closest acceptable text, how often an acceptable
// text is among the first phrase-by-phrase choices, and conversion time.

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/MixedConverter.h"
#include "Core/Japanese/RomajiTable.h"

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

std::wstring Widen(std::string_view utf8) {
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string Narrow(std::wstring_view text) {
#if defined(_WIN32)
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr,
                        nullptr);
    return out;
#else
    return std::string(text.begin(), text.end());
#endif
}

std::size_t EditDistance(std::wstring_view a, std::wstring_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

struct Row {
    std::string source;
    std::string id;
    std::wstring reading;
    std::vector<std::wstring> expected;
};

std::vector<Row> LoadCorpus(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<Row> rows;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
        // Readings files: source, id, context, reading, expected...
        // Keys files: source, id, keys, expected...
        const bool keys = path.filename().string().find("_keys") != std::string::npos;
        const std::size_t text = keys ? 2 : 3;
        if (fields.size() < text + 2) continue;
        Row row{fields[0], fields[1], Widen(fields[text]), {}};
        for (std::size_t i = text + 1; i < fields.size(); ++i) row.expected.push_back(Widen(fields[i]));
        rows.push_back(std::move(row));
    }
    return rows;
}

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

double Percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(q * values.size()))];
}

bool HasLatinLetter(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
    });
}

int RunKeys(const ConnectionMatrix& matrix, const JapaneseDictionary& dictionary, const JapaneseConverter& converter,
            const std::filesystem::path& corpus, const std::filesystem::path& romaji,
            const std::filesystem::path& englishPack, bool useMixed,
            tekito::japanese::MixedConverter::Costs costs, std::size_t showMisses) {
    const auto table = tekito::japanese::RomajiTable::Load(romaji);
    tekito::japanese::EnglishWords english;
    if (!table || !english.Open(englishPack)) {
        std::cerr << "could not open the romaji table or the English words\n";
        return 1;
    }
    const tekito::japanese::MixedConverter mixed(dictionary, matrix, converter, *table, english, costs);
    tekito::japanese::JapaneseComposer composer(table.get());
    composer.SetConverter(&converter);
    if (useMixed) composer.SetMixedConverter(&mixed);

    struct KeyTotals {
        std::size_t rows{0}, exact{0}, inList{0}, english{0}, errors{0}, characters{0};
        std::vector<double> microseconds;
        std::vector<double> listMicroseconds;
    };
    std::map<std::string, KeyTotals> totals;
    std::size_t shown = 0;
    // Keys files share the layout source, id, keys, expected...: LoadCorpus
    // reads keys where it reads readings.
    for (const auto& row : LoadCorpus(corpus)) {
        composer.Clear();
        for (const wchar_t key : row.reading) composer.Insert(key);
        const auto start = Clock::now();
        composer.Convert();
        const double elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        const std::wstring output = composer.Preedit();
        std::size_t distance = SIZE_MAX, length = 0;
        for (const auto& expected : row.expected) {
            const std::size_t d = EditDistance(output, expected);
            if (d < distance) {
                distance = d;
                length = expected.size();
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
        const std::size_t phraseCount = composer.Segments().size();
        for (std::size_t f = 0; !inList && f < phraseCount; ++f) {
            composer.MoveFocus(-static_cast<int>(phraseCount));
            composer.MoveFocus(static_cast<int>(f));
            composer.NextCandidate();
            composer.PreviousCandidate();
            const auto segments = composer.Segments();
            const auto* candidates = composer.FocusedCandidates();
            for (std::size_t j = 0; candidates && j < std::min<std::size_t>(9, candidates->size()) && !inList; ++j) {
                std::wstring text;
                for (std::size_t s = 0; s < segments.size(); ++s) {
                    text += s == f ? (*candidates)[j].text : segments[s].text;
                }
                inList = std::find(row.expected.begin(), row.expected.end(), text) != row.expected.end();
            }
        }
        for (KeyTotals* t : {&totals[row.source], &totals["all"]}) {
            ++t->rows;
            if (distance == 0) ++t->exact;
            if (inList) ++t->inList;
            t->listMicroseconds.push_back(listElapsed);
            if (HasLatinLetter(output)) ++t->english;
            t->errors += distance;
            t->characters += length;
            t->microseconds.push_back(elapsed);
        }
        if (distance != 0 && shown < showMisses) {
            ++shown;
            std::cout << "miss " << row.source << ":" << row.id << "  " << Narrow(output) << "  (want "
                      << Narrow(row.expected.front()) << ")\n";
        }
    }
    std::cout << std::fixed << std::setprecision(1);
    for (const auto& [source, t] : totals) {
        std::cout << source << ": rows=" << t.rows << " top1=" << 100.0 * t.exact / t.rows << "%"
                  << " in_list=" << 100.0 * t.inList / t.rows << "%"
                  << " english=" << 100.0 * t.english / t.rows << "%"
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
    std::filesystem::path pack, corpus, keysCorpus, romaji, englishPack;
    std::size_t showMisses = 0;
    bool useMixed = true;
    tekito::japanese::MixedConverter::Costs costs;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (arg == "--keys-corpus" && i + 1 < argc) keysCorpus = argv[++i];
        else if (arg == "--romaji" && i + 1 < argc) romaji = argv[++i];
        else if (arg == "--english" && i + 1 < argc) englishPack = argv[++i];
        else if (arg == "--no-mixed") useMixed = false;
        else if (arg == "--english-base" && i + 1 < argc) costs.englishBase = std::stoll(argv[++i]);
        else if (arg == "--english-per-score" && i + 1 < argc) costs.englishPerScore = std::stoll(argv[++i]);
        else if (arg == "--language-switch" && i + 1 < argc) costs.languageSwitch = std::stoll(argv[++i]);
        else if (arg == "--typo" && i + 1 < argc) costs.typo = std::stoll(argv[++i]);
        else if (arg == "--show-misses" && i + 1 < argc) showMisses = std::stoul(argv[++i]);
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
        const JapaneseConverter converter(dictionary, matrix);
        return RunKeys(matrix, dictionary, converter, keysCorpus, romaji, englishPack, useMixed, costs, showMisses);
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
    const JapaneseConverter converter(dictionary, matrix);
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
