#include "Core/Japanese/LetterOdds.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace tekito::japanese {
namespace {

constexpr int kSymbols = 28;

int SymbolOf(char c) noexcept {
    if (c == '^') return 0;
    if (c == '$') return kSymbols - 1;
    if (c >= 'a' && c <= 'z') return c - 'a' + 1;
    return -1;
}

}  // namespace

bool LetterOdds::Open(const std::filesystem::path& packDirectory) {
    odds_.clear();
    std::ifstream in(packDirectory / L"odds.tsv", std::ios::binary);
    if (!in) return false;
    std::vector<float> odds(kSymbols * kSymbols * kSymbols, 0.0f);
    std::size_t rows = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.size() < 5 || line[3] != '\t') continue;
        const int a = SymbolOf(line[0]), b = SymbolOf(line[1]), c = SymbolOf(line[2]);
        if (a < 0 || b < 0 || c < 0) continue;
        odds[(a * kSymbols + b) * kSymbols + c] = std::strtof(line.c_str() + 4, nullptr);
        ++rows;
    }
    if (rows != odds.size()) return false;
    odds_ = std::move(odds);
    return true;
}

double LetterOdds::Score(std::wstring_view lower) const noexcept {
    if (odds_.empty()) return 0.0;
    double score = 0.0;
    int a = 0, b = 0;
    for (std::size_t i = 0; i <= lower.size(); ++i) {
        int c = kSymbols - 1;
        if (i < lower.size()) {
            if (lower[i] < L'a' || lower[i] > L'z') return 0.0;
            c = static_cast<int>(lower[i] - L'a') + 1;
        }
        score += odds_[(a * kSymbols + b) * kSymbols + c];
        a = b;
        b = c;
    }
    return score;
}

}  // namespace tekito::japanese
