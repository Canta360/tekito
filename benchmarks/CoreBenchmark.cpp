#include "Core/CandidateEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Scenario {
    std::string name;
    std::wstring input;
    std::wstring preceding;
    std::wstring following;
};

struct Summary {
    double p50Us{};
    double p95Us{};
    double p99Us{};
    double meanUs{};
    std::size_t candidateCount{};
};

volatile std::size_t g_candidateSink = 0;

std::optional<std::size_t> CurrentRssKiB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(counters.WorkingSetSize / 1024);
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            std::size_t kib = 0;
            status >> kib;
            return kib;
        }
        std::string remainder;
        std::getline(status, remainder);
    }
#endif
    return std::nullopt;
}

double Percentile(std::vector<double> samples, double quantile) {
    if (samples.empty()) return 0.0;
    const auto position = static_cast<std::size_t>(
        std::ceil(quantile * static_cast<double>(samples.size())) - 1.0);
    const auto index = std::min(position, samples.size() - 1);
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(index),
                     samples.end());
    return samples[index];
}

tekito::ConversionRequest RequestFor(const Scenario& scenario,
                                     const tekito::ConversionOptions& options) {
    tekito::ConversionRequest request;
    request.rawText = scenario.input;
    request.context.precedingText = scenario.preceding;
    request.context.followingText = scenario.following;
    request.options = options;
    return request;
}

Summary MeasureScenario(const tekito::IConversionEngine& engine, const Scenario& scenario,
                        const tekito::ConversionOptions& options, int iterations) {
    constexpr int kWarmupIterations = 6;
    const auto request = RequestFor(scenario, options);
    for (int index = 0; index < kWarmupIterations; ++index) {
        const auto result = engine.Convert(request);
        g_candidateSink += result.candidates.size();
    }

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::size_t candidates = 0;
    for (int index = 0; index < iterations; ++index) {
        const auto started = Clock::now();
        const auto result = engine.Convert(request);
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - started);
        samples.push_back(elapsed.count());
        candidates += result.candidates.size();
        g_candidateSink += result.candidates.size();
    }
    const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
    return {Percentile(samples, 0.50), Percentile(samples, 0.95), Percentile(samples, 0.99),
            total / static_cast<double>(samples.size()), candidates / samples.size()};
}

Summary MeasureFirstRequest(const Scenario& scenario, const tekito::ConversionOptions& options,
                            int iterations) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    std::size_t candidates = 0;
    const auto request = RequestFor(scenario, options);
    for (int index = 0; index < iterations; ++index) {
        tekito::CandidateEngine engine;
        const auto started = Clock::now();
        const auto result = engine.Convert(request);
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - started);
        samples.push_back(elapsed.count());
        candidates += result.candidates.size();
        g_candidateSink += result.candidates.size();
    }
    const auto total = std::accumulate(samples.begin(), samples.end(), 0.0);
    return {Percentile(samples, 0.50), Percentile(samples, 0.95), Percentile(samples, 0.99),
            total / static_cast<double>(samples.size()), candidates / samples.size()};
}

double MeasureConstructionUs(int iterations) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int index = 0; index < iterations; ++index) {
        const auto started = Clock::now();
        tekito::CandidateEngine engine;
        const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - started);
        samples.push_back(elapsed.count());
        g_candidateSink += engine.Generate(L"tekito").size();
    }
    return Percentile(samples, 0.50);
}

void PrintSummary(const std::string& profile, const Scenario& scenario, const Summary& summary) {
    std::cout << profile << ',' << scenario.name << ',' << std::fixed << std::setprecision(1)
              << summary.p50Us << ',' << summary.p95Us << ',' << summary.p99Us << ','
              << summary.meanUs << ',' << summary.candidateCount << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    int iterations = 60;
    if (argc == 3 && std::string_view(argv[1]) == "--iterations") {
        try {
            iterations = std::clamp(std::stoi(argv[2]), 10, 10000);
        } catch (...) {
            std::cerr << "Invalid --iterations value\n";
            return 2;
        }
    } else if (argc != 1) {
        std::cerr << "Usage: tekito_core_benchmark [--iterations N]\n";
        return 2;
    }

    const std::vector<Scenario> scenarios = {
        {"exact_input_start", L"the", L"", L""},
        {"exact_preceding", L"the", L"in", L""},
        {"exact_common", L"the", L"in", L"morning"},
        {"single_typo", L"helo", L"please", L"there"},
        {"transpose_typo", L"wrold", L"hello", L"today"},
        {"long_typo", L"definitely", L"I", L"agree"},
        {"slang", L"gonna", L"I", L"leave"},
        {"emoji", L"grinning", L"feeling", L"today"},
        {"proper_noun", L"tokyo", L"visit", L"soon"},
        {"completion", L"th", L"in", L"morning"},
    };

    tekito::ConversionOptions fullOptions;
    tekito::ConversionOptions noContextOptions = fullOptions;
    noContextOptions.contextSuggestionsEnabled = false;
    tekito::ConversionOptions noCompletionOptions = fullOptions;
    noCompletionOptions.completionEnabled = false;
    tekito::ConversionOptions lowSpecOptions = fullOptions;
    lowSpecOptions.contextSuggestionsEnabled = false;
    lowSpecOptions.completionEnabled = false;

    std::cout << "# TEKITO core benchmark\n";
    std::cout << "# iterations=" << iterations << '\n';
    std::cout << "# construction_p50_us=" << std::fixed << std::setprecision(1)
              << MeasureConstructionUs(5) << '\n';

    tekito::CandidateEngine engine;
    const auto initialRss = CurrentRssKiB();
    std::cout << "# rss_after_engine_init_kib="
              << (initialRss ? std::to_string(*initialRss) : "unavailable") << '\n';
    std::cout << "profile,scenario,p50_us,p95_us,p99_us,mean_us,mean_candidates\n";

    for (const auto* scenario : {&scenarios[0], &scenarios[1], &scenarios[2]}) {
        PrintSummary("first_request", *scenario,
                     MeasureFirstRequest(*scenario, fullOptions, std::min(iterations, 20)));
    }

    for (const auto& scenario : scenarios) {
        PrintSummary("full", scenario, MeasureScenario(engine, scenario, fullOptions, iterations));
    }
    for (const auto& scenario : scenarios) {
        PrintSummary("no_context", scenario,
                     MeasureScenario(engine, scenario, noContextOptions, iterations));
    }
    for (const auto& scenario : scenarios) {
        PrintSummary("no_completion", scenario,
                     MeasureScenario(engine, scenario, noCompletionOptions, iterations));
    }
    for (const auto& scenario : scenarios) {
        PrintSummary("low_spec", scenario, MeasureScenario(engine, scenario, lowSpecOptions, iterations));
    }

    const auto finalRss = CurrentRssKiB();
    std::cout << "# rss_after_workload_kib="
              << (finalRss ? std::to_string(*finalRss) : "unavailable") << '\n';
    std::cout << "# candidate_sink=" << g_candidateSink << '\n';
    return 0;
}
