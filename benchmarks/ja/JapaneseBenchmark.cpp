// Speed and a first accuracy figure for Japanese conversion over the
// japanese-core pack (scripts/ja/build-japanese-packs.py), on Mozc's
// evaluation.tsv.
//
// tekito_ja_benchmark --pack <dir> --eval <evaluation.tsv> [--show-misses]

#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using tekito::japanese::ConnectionMatrix;
using tekito::japanese::JapaneseConverter;
using tekito::japanese::JapaneseDictionary;
using tekito::japanese::Phrase;

volatile std::size_t g_sink = 0;

double Microseconds(Clock::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

std::size_t WorkingSetKiB() {
    PROCESS_MEMORY_COUNTERS counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters));
    return counters.WorkingSetSize / 1024;
}

std::wstring Widen(std::string_view utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string Narrow(std::wstring_view text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr,
                        nullptr);
    return out;
}

std::wstring Best(const std::vector<Phrase>& phrases) {
    std::wstring out;
    for (const auto& phrase : phrases) {
        if (!phrase.candidates.empty()) out += phrase.candidates.front().text;
    }
    return out;
}

struct Percentiles {
    double p50, p95, p99, max;
};

Percentiles Summarize(std::vector<double> samples) {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        return samples[std::min(samples.size() - 1, static_cast<std::size_t>(q * samples.size()))];
    };
    return {at(0.50), at(0.95), at(0.99), samples.back()};
}

void Print(const char* name, const Percentiles& p) {
    std::cout << name << "_us p50=" << p.p50 << " p95=" << p.p95 << " p99=" << p.p99 << " max=" << p.max << "\n";
}

struct EvalRow {
    std::wstring input, argument;
    std::string command;
};

std::vector<EvalRow> LoadEval(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<EvalRow> rows;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
        if (fields.size() < 5) continue;
        rows.push_back({Widen(fields[1]), Widen(fields[4]), fields[3]});
    }
    return rows;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path pack, eval;
    bool showMisses = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        if (arg == L"--pack" && i + 1 < argc) pack = argv[++i];
        else if (arg == L"--eval" && i + 1 < argc) eval = argv[++i];
        else if (arg == L"--show-misses") showMisses = true;
    }
    if (pack.empty() || eval.empty()) {
        std::cerr << "usage: tekito_ja_benchmark --pack <dir> --eval <evaluation.tsv> [--show-misses]\n";
        return 2;
    }
    SetConsoleOutputCP(CP_UTF8);

    const std::size_t baseKiB = WorkingSetKiB();
    const auto openStart = Clock::now();
    JapaneseDictionary dictionary;
    ConnectionMatrix matrix;
    if (!dictionary.Open(pack / L"dictionary.bin") || !matrix.Open(pack / L"connection.bin")) {
        std::cerr << "could not open the japanese-core pack in the given directory\n";
        return 1;
    }
    std::cout << "open_us " << Microseconds(Clock::now() - openStart) << "\n";
    const JapaneseConverter converter(dictionary, matrix);
    const auto rows = LoadEval(eval);

    std::size_t exactTotal = 0, exactHit = 0, matchTotal = 0, matchHit = 0;
    for (const auto& row : rows) {
        const std::wstring output = Best(converter.Convert(row.input));
        if (row.command == "Conversion Expected") {
            ++exactTotal;
            if (output == row.argument) {
                ++exactHit;
            } else if (showMisses) {
                std::cout << "miss " << Narrow(row.input) << " -> " << Narrow(output) << " (want "
                          << Narrow(row.argument) << ")\n";
            }
        } else if (row.command == "Conversion Match" || row.command == "Conversion Not Match") {
            ++matchTotal;
            const bool contains = output.find(row.argument) != std::wstring::npos;
            if (contains == (row.command == "Conversion Match")) ++matchHit;
        }
    }
    std::cout << "top1_exact " << exactHit << "/" << exactTotal << "\n";
    std::cout << "match_checks " << matchHit << "/" << matchTotal << "\n";

    // Converting each input, and each of its prefixes (as if converting
    // after every key), then long inputs made by joining rows.
    std::vector<double> full, perPrefix;
    for (int round = 0; round < 3; ++round) {
        for (const auto& row : rows) {
            auto start = Clock::now();
            g_sink = g_sink + converter.Convert(row.input).size();
            full.push_back(Microseconds(Clock::now() - start));
            if (round > 0) continue;
            for (std::size_t k = 1; k <= row.input.size(); ++k) {
                start = Clock::now();
                g_sink = g_sink + converter.Convert(std::wstring_view(row.input).substr(0, k)).size();
                perPrefix.push_back(Microseconds(Clock::now() - start));
            }
        }
    }
    Print("convert_full", Summarize(full));
    // Prediction after each key, from the second character.
    std::vector<double> predictions;
    for (const auto& row : rows) {
        for (std::size_t k = 2; k <= row.input.size(); ++k) {
            const auto start = Clock::now();
            g_sink = g_sink + converter.Predict(std::wstring_view(row.input).substr(0, k), 5).size();
            predictions.push_back(Microseconds(Clock::now() - start));
        }
    }
    Print("predict_every_prefix", Summarize(predictions));
    Print("convert_every_prefix", Summarize(perPrefix));
    for (std::size_t target : {40u, 100u}) {
        std::vector<double> samples;
        std::wstring joined;
        for (const auto& row : rows) {
            joined += row.input;
            if (joined.size() < target) continue;
            joined.resize(target);
            const auto start = Clock::now();
            g_sink = g_sink + converter.Convert(joined).size();
            samples.push_back(Microseconds(Clock::now() - start));
            joined.clear();
        }
        const std::string name = "convert_" + std::to_string(target) + "_chars";
        Print(name.c_str(), Summarize(samples));
    }
    std::cout << "working_set_kib base=" << baseKiB << " after=" << WorkingSetKiB() << "\n";
    return 0;
}
