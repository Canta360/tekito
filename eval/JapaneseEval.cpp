// Offline accuracy of Japanese conversion: converts each reading of
// eval/generated/japanese_eval.tsv (scripts/prepare-japanese-eval.py) with
// the japanese-core pack and compares the first conversion with the
// acceptable ones.
//
// tekito_ja_eval --pack <data/japanese-core> --corpus <japanese_eval.tsv> [--show-misses N]
//
// Per source: sentences whose first conversion is acceptable, the character
// error rate against the closest acceptable text, how often an acceptable
// text is among the first phrase-by-phrase choices, and conversion time.

#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"

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
        if (fields.size() < 5) continue;
        Row row{fields[0], fields[1], Widen(fields[3]), {}};
        for (std::size_t i = 4; i < fields.size(); ++i) row.expected.push_back(Widen(fields[i]));
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

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path pack, corpus;
    std::size_t showMisses = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (arg == "--show-misses" && i + 1 < argc) showMisses = std::stoul(argv[++i]);
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
