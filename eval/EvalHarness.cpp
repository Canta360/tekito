// Offline accuracy/safety/latency evaluation against the real CandidateEngine.
// Reads the flat TSV produced by scripts/prepare-eval-corpus.py; does not modify
// runtime candidate generation or ranking.
#include "Core/AutoApplyPolicy.h"
#include "Core/ConversionEngine.h"
#include "Core/CandidateEngine.h"
#include "Core/DataPackPath.h"
#include "Core/ExternalLexiconProvider.h"
#include "EnglishTypingStudy.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
volatile std::size_t g_sink = 0;

std::wstring Wide(std::string_view text) {
#if defined(_WIN32)
    if (text.size() <= static_cast<std::size_t>(INT_MAX)) {
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                 static_cast<int>(text.size()), nullptr, 0);
        if (length > 0) {
            std::wstring result(static_cast<std::size_t>(length), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()), result.data(), length) == length) {
                return result;
            }
        }
    }
#endif
    std::wstring result;
    result.reserve(text.size());
    for (const unsigned char ch : text) result.push_back(static_cast<wchar_t>(ch));
    return result;
}

struct Row {
    std::string source;
    std::string split;
    std::string category;
    std::string expectedAction;
    std::wstring rawToken;
    std::wstring expectedToken;
    std::wstring leftContext;
    std::wstring rightContext;
};

std::vector<std::string> SplitTsv(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

std::vector<Row> LoadCorpus(const std::filesystem::path& path, std::string_view splitFilter) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open corpus file: " << path << '\n';
        return {};
    }

    std::vector<Row> rows;
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto fields = SplitTsv(line);
        if (fields.size() != 9) continue;
        // source, split, category, typo_operation, expected_action, raw_token,
        // expected_token, left_context, right_context
        if (!splitFilter.empty() && fields[1] != splitFilter) continue;
        Row row;
        row.source = fields[0];
        row.split = fields[1];
        row.category = fields[2];
        row.expectedAction = fields[4];
        row.rawToken = Wide(fields[5]);
        row.expectedToken = Wide(fields[6]);
        row.leftContext = Wide(fields[7]);
        row.rightContext = Wide(fields[8]);
        rows.push_back(std::move(row));
    }
    return rows;
}

struct AccuracyBucket {
    std::size_t total{0};
    std::size_t top1{0};
    std::size_t top9{0};
    std::size_t rankedLow{0};    // found beyond rank 9 (rank-loss, not a candidate-generation miss)
    std::size_t vocabGap{0};     // not found; expected_token also fails a direct self-lookup
    std::size_t generationMiss{0};  // not found; expected_token IS reachable via a direct self-lookup
};

struct SafetyBucket {
    std::size_t total{0};
    std::size_t falseCorrections{0};
};

double Percentile(std::vector<double> samples, double quantile) {
    if (samples.empty()) return 0.0;
    const auto position = static_cast<std::size_t>(
        std::ceil(quantile * static_cast<double>(samples.size())) - 1.0);
    const auto index = std::min(position, samples.size() - 1);
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index),
                     samples.end());
    return samples[index];
}

struct LatencyStats {
    double p50{0};
    double p95{0};
    double p99{0};
    double meanUs{0};
    std::size_t samples{0};
};

LatencyStats Summarize(std::vector<double> samplesUs) {
    LatencyStats stats;
    stats.samples = samplesUs.size();
    if (samplesUs.empty()) return stats;
    stats.meanUs = std::accumulate(samplesUs.begin(), samplesUs.end(), 0.0) /
                   static_cast<double>(samplesUs.size());
    stats.p50 = Percentile(samplesUs, 0.50);
    stats.p95 = Percentile(samplesUs, 0.95);
    stats.p99 = Percentile(samplesUs, 0.99);
    return stats;
}

std::string BucketKey(const Row& row) { return row.source + "/" + row.category; }

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path corpusPath;
    std::string splitFilter = "test";
    std::size_t latencySampleSize = 200;
    std::size_t maxRows = 0;  // 0 = unlimited
    // --sentences: English Auto on real text (EnglishTypingStudy.h).
    EnglishStudyOptions study;

    const auto usage = [] {
        std::cerr << "Usage: tekito_core_eval --corpus PATH [--split test|validation|train|all] "
                     "[--latency-sample N] [--max-rows N]\n"
                     "       tekito_core_eval --sentences LEIPZIG_SENTENCES [--corpus PATH] "
                     "[--max-sentences N] [--show-examples N]\n";
    };

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--corpus" && index + 1 < argc) {
            corpusPath = argv[++index];
        } else if (arg == "--split" && index + 1 < argc) {
            splitFilter = argv[++index];
            if (splitFilter == "all") splitFilter.clear();
        } else if (arg == "--latency-sample" && index + 1 < argc) {
            latencySampleSize = static_cast<std::size_t>(std::stoul(argv[++index]));
        } else if (arg == "--max-rows" && index + 1 < argc) {
            maxRows = static_cast<std::size_t>(std::stoul(argv[++index]));
        } else if (arg == "--sentences" && index + 1 < argc) {
            study.sentences = argv[++index];
        } else if (arg == "--max-sentences" && index + 1 < argc) {
            study.maxSentences = static_cast<std::size_t>(std::stoul(argv[++index]));
        } else if (arg == "--latency") {
            study.latency = true;
        } else if (arg == "--probe" && index + 1 < argc) {
            study.probe = argv[++index];
        } else if (arg == "--show-examples" && index + 1 < argc) {
            study.showExamples = static_cast<std::size_t>(std::stoul(argv[++index]));
        } else {
            usage();
            return 2;
        }
    }

    if (corpusPath.empty() && study.sentences.empty() && study.probe.empty()) {
        usage();
        return 2;
    }

    const auto dataPackRoot = tekito::ExternalLexiconProvider::DataPackRoot();
    if (dataPackRoot.empty() ||
        !std::filesystem::exists(tekito::FindDataPack(dataPackRoot, L"standard-english") / L"lexicon.txt")) {
        std::cerr << "TEKITO_DATA_PACK_DIR does not point at a valid data pack directory ("
                  << dataPackRoot << "). Set it to the repository's data/ directory.\n";
        return 2;
    }
    if (!study.sentences.empty() || !study.probe.empty()) {
        study.corpus = corpusPath;
        return RunEnglishStudy(study);
    }

    auto rows = LoadCorpus(corpusPath, splitFilter);
    if (rows.empty()) {
        std::cerr << "No rows loaded from " << corpusPath << " (split=" << splitFilter << ")\n";
        return 2;
    }
    if (maxRows > 0 && rows.size() > maxRows) rows.resize(maxRows);

    tekito::CandidateEngine engine;
    tekito::AutoApplyPolicy autoApply;

    std::map<std::string, AccuracyBucket> accuracy;
    std::map<std::string, SafetyBucket> safety;
    std::vector<double> fullWordLatencyUs;
    std::vector<double> keystrokeLatencyUs;
    fullWordLatencyUs.reserve(rows.size());

    std::size_t latencyRowsSampled = 0;
    const auto runStarted = Clock::now();
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const auto& row = rows[rowIndex];
        if (rowIndex > 0 && rowIndex % 200 == 0) {
            const auto elapsedSec =
                std::chrono::duration<double>(Clock::now() - runStarted).count();
            std::cerr << "... " << rowIndex << '/' << rows.size() << " rows ("
                      << std::fixed << std::setprecision(1) << elapsedSec << "s elapsed)\n";
        }
        tekito::ConversionRequest request;
        request.rawText = row.rawToken;
        request.context.precedingText = row.leftContext;
        request.context.followingText = row.rightContext;

        const auto started = Clock::now();
        const auto result = engine.Convert(request);
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - started);
        fullWordLatencyUs.push_back(elapsed.count());
        g_sink += result.candidates.size();

        if (row.expectedAction == "auto_apply_eligible") {
            auto& bucket = accuracy[BucketKey(row)];
            bucket.total += 1;
            const auto match = std::find_if(
                result.candidates.begin(), result.candidates.end(),
                [&](const tekito::Candidate& candidate) { return candidate.text == row.expectedToken; });
            if (match == result.candidates.end()) {
                // Not surfaced for this raw token. Distinguish "not in any lexicon the
                // engine uses" from "in the lexicon, but the fuzzy search from this raw
                // token missed it" by asking the engine to look the correct word up
                // directly (its own exact-match lookup, independent of the typo).
                tekito::ConversionRequest selfLookup;
                selfLookup.rawText = row.expectedToken;
                const auto selfResult = engine.Convert(selfLookup);
                const auto selfMatch = std::any_of(
                    selfResult.candidates.begin(), selfResult.candidates.end(),
                    [&](const tekito::Candidate& candidate) { return candidate.text == row.expectedToken; });
                if (selfMatch) {
                    bucket.generationMiss += 1;
                } else {
                    bucket.vocabGap += 1;
                }
            } else {
                const auto rank = std::distance(result.candidates.begin(), match);
                if (rank == 0) bucket.top1 += 1;
                if (rank < 9) bucket.top9 += 1;
                else bucket.rankedLow += 1;
            }
        } else if (row.expectedAction == "keep_raw" || row.expectedAction == "suggest_only") {
            auto& bucket = safety[BucketKey(row)];
            bucket.total += 1;
            const auto selected = autoApply.SelectForBoundary(row.rawToken, result.candidates);
            if (selected) bucket.falseCorrections += 1;
        }

        if (latencyRowsSampled < latencySampleSize && !row.rawToken.empty()) {
            latencyRowsSampled += 1;
            for (std::size_t length = 1; length <= row.rawToken.size(); ++length) {
                tekito::ConversionRequest prefixRequest = request;
                prefixRequest.rawText = row.rawToken.substr(0, length);
                const auto keyStarted = Clock::now();
                const auto prefixResult = engine.Convert(prefixRequest);
                const auto keyElapsed =
                    std::chrono::duration<double, std::micro>(Clock::now() - keyStarted);
                keystrokeLatencyUs.push_back(keyElapsed.count());
                g_sink += prefixResult.candidates.size();
            }
        }
    }

    std::cout << "# TEKITO eval harness\n";
    std::cout << "# corpus=" << corpusPath << '\n';
    std::cout << "# split=" << (splitFilter.empty() ? "all" : splitFilter) << '\n';
    std::cout << "# rows=" << rows.size() << '\n';

    std::cout << "accuracy_category,total,top1,top1_rate,top9,top9_rate,"
                 "ranked_low,ranked_low_rate,generation_miss,generation_miss_rate,"
                 "vocab_gap,vocab_gap_rate\n";
    std::size_t accuracyTotal = 0, accuracyTop1 = 0, accuracyTop9 = 0;
    std::size_t accuracyRankedLow = 0, accuracyGenerationMiss = 0, accuracyVocabGap = 0;
    for (const auto& [key, bucket] : accuracy) {
        accuracyTotal += bucket.total;
        accuracyTop1 += bucket.top1;
        accuracyTop9 += bucket.top9;
        accuracyRankedLow += bucket.rankedLow;
        accuracyGenerationMiss += bucket.generationMiss;
        accuracyVocabGap += bucket.vocabGap;
        const auto rate = [&](std::size_t count) {
            return bucket.total ? static_cast<double>(count) / bucket.total : 0.0;
        };
        std::cout << key << ',' << bucket.total << ',' << bucket.top1 << ',' << std::fixed
                  << std::setprecision(4) << rate(bucket.top1) << ',' << bucket.top9 << ','
                  << rate(bucket.top9) << ',' << bucket.rankedLow << ',' << rate(bucket.rankedLow)
                  << ',' << bucket.generationMiss << ',' << rate(bucket.generationMiss) << ','
                  << bucket.vocabGap << ',' << rate(bucket.vocabGap) << '\n';
    }

    std::cout << "safety_category,total,false_corrections,false_correction_rate\n";
    std::size_t safetyTotal = 0, safetyFalseCorrections = 0;
    for (const auto& [key, bucket] : safety) {
        safetyTotal += bucket.total;
        safetyFalseCorrections += bucket.falseCorrections;
        std::cout << key << ',' << bucket.total << ',' << bucket.falseCorrections << ',' << std::fixed
                  << std::setprecision(4)
                  << (bucket.total ? static_cast<double>(bucket.falseCorrections) / bucket.total : 0.0)
                  << '\n';
    }

    const auto fullWordLatency = Summarize(fullWordLatencyUs);
    const auto keystrokeLatency = Summarize(keystrokeLatencyUs);

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "# overall_top1_rate="
              << (accuracyTotal ? static_cast<double>(accuracyTop1) / accuracyTotal : 0.0) << '\n';
    std::cout << "# overall_top9_rate="
              << (accuracyTotal ? static_cast<double>(accuracyTop9) / accuracyTotal : 0.0) << '\n';
    std::cout << "# overall_ranked_low_rate="
              << (accuracyTotal ? static_cast<double>(accuracyRankedLow) / accuracyTotal : 0.0)
              << '\n';
    std::cout << "# overall_generation_miss_rate="
              << (accuracyTotal ? static_cast<double>(accuracyGenerationMiss) / accuracyTotal : 0.0)
              << '\n';
    std::cout << "# overall_vocab_gap_rate="
              << (accuracyTotal ? static_cast<double>(accuracyVocabGap) / accuracyTotal : 0.0) << '\n';
    std::cout << "# overall_false_correction_rate="
              << (safetyTotal ? static_cast<double>(safetyFalseCorrections) / safetyTotal : 0.0)
              << '\n';
    std::cout << std::setprecision(1);
    std::cout << "# full_word_latency_us_p50=" << fullWordLatency.p50 << '\n';
    std::cout << "# full_word_latency_us_p95=" << fullWordLatency.p95 << '\n';
    std::cout << "# full_word_latency_us_p99=" << fullWordLatency.p99 << '\n';
    std::cout << "# per_keystroke_latency_us_p50=" << keystrokeLatency.p50
              << " (n=" << keystrokeLatency.samples << ", rows=" << latencyRowsSampled << ")\n";
    std::cout << "# per_keystroke_latency_us_p95=" << keystrokeLatency.p95 << '\n';
    std::cout << "# per_keystroke_latency_us_p99=" << keystrokeLatency.p99 << '\n';
    std::cout << "# candidate_sink=" << g_sink << '\n';

    return 0;
}
