#include "Core/TypoModel.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <cstdlib>
#include <limits>

namespace tekito {
namespace {

std::size_t EditDistance(std::wstring_view left, std::wstring_view right) noexcept {
    std::array<std::size_t, 32> beforePrevious{};
    std::array<std::size_t, 32> previous{};
    std::array<std::size_t, 32> current{};
    if (left.size() >= previous.size() || right.size() >= current.size()) {
        return std::numeric_limits<std::size_t>::max();
    }
    for (std::size_t i = 0; i <= right.size(); ++i) previous[i] = i;
    for (std::size_t i = 1; i <= left.size(); ++i) {
        current[0] = i;
        for (std::size_t j = 1; j <= right.size(); ++j) {
            const std::size_t cost = left[i - 1] == right[j - 1] ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                                   previous[j - 1] + cost});
            if (i > 1 && j > 1 && left[i - 1] == right[j - 2] &&
                left[i - 2] == right[j - 1]) {
                current[j] = std::min(current[j], beforePrevious[j - 2] + 1);
            }
        }
        beforePrevious = previous;
        previous = current;
    }
    return previous[right.size()];
}

constexpr double kSubstitutionCost = 1.0;
constexpr double kInsertDeleteCost = 1.15;
constexpr double kTranspositionCost = 1.0;

double WeightedEditDistance(std::wstring_view left, std::wstring_view right) noexcept {
    std::array<double, 32> beforePrevious{};
    std::array<double, 32> previous{};
    std::array<double, 32> current{};
    if (left.size() >= previous.size() || right.size() >= current.size()) {
        return static_cast<double>(std::max(left.size(), right.size())) * kInsertDeleteCost;
    }
    for (std::size_t i = 0; i <= right.size(); ++i) {
        previous[i] = static_cast<double>(i) * kInsertDeleteCost;
    }
    for (std::size_t i = 1; i <= left.size(); ++i) {
        current[0] = static_cast<double>(i) * kInsertDeleteCost;
        for (std::size_t j = 1; j <= right.size(); ++j) {
            const double substitution = left[i - 1] == right[j - 1] ? 0.0 : kSubstitutionCost;
            current[j] = std::min({previous[j] + kInsertDeleteCost, current[j - 1] + kInsertDeleteCost,
                                   previous[j - 1] + substitution});
            if (i > 1 && j > 1 && left[i - 1] == right[j - 2] &&
                left[i - 2] == right[j - 1]) {
                current[j] = std::min(current[j], beforePrevious[j - 2] + kTranspositionCost);
            }
        }
        beforePrevious = previous;
        previous = current;
    }
    return previous[right.size()];
}

bool IsRepeatedKeyVariant(std::wstring_view input, std::wstring_view target) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    bool skipped = false;
    while (i < input.size()) {
        if (j < target.size() && input[i] == target[j]) {
            ++i;
            ++j;
            continue;
        }
        if (i > 0 && input[i] == input[i - 1]) {
            skipped = true;
            ++i;
            continue;
        }
        return false;
    }
    return skipped && j == target.size();
}

bool IsTranspositionVariant(std::wstring_view input, std::wstring_view target) noexcept {
    if (input.size() != target.size()) return false;

    std::size_t index = 0;
    while (index < input.size() && input[index] == target[index]) ++index;
    return index + 1 < input.size() &&
           input[index] == target[index + 1] &&
           input[index + 1] == target[index] &&
           input.substr(index + 2) == target.substr(index + 2);
}

bool IsOneInsertedCharacterVariant(std::wstring_view longer,
                                   std::wstring_view shorter) noexcept {
    if (longer.size() != shorter.size() + 1) return false;

    std::size_t i = 0;
    std::size_t j = 0;
    bool skipped = false;
    while (i < longer.size()) {
        if (j < shorter.size() && longer[i] == shorter[j]) {
            ++i;
            ++j;
            continue;
        }
        if (skipped) return false;
        skipped = true;
        ++i;
    }
    return true;
}

struct KeyPosition {
    int row;
    int column;
};

KeyPosition QwertyPosition(wchar_t character) noexcept {
    const auto lower = static_cast<wchar_t>(std::towlower(character));
    constexpr std::wstring_view rows[] = {L"qwertyuiop", L"asdfghjkl", L"zxcvbnm"};
    for (int row = 0; row < 3; ++row) {
        const auto index = rows[row].find(lower);
        if (index != std::wstring_view::npos) {
            return {row, static_cast<int>(index)};
        }
    }
    return {-1, -1};
}

bool IsQwertyNeighbor(wchar_t left, wchar_t right) noexcept {
    const auto leftPosition = QwertyPosition(left);
    const auto rightPosition = QwertyPosition(right);
    if (leftPosition.row < 0 || rightPosition.row < 0) return false;
    const int rowDelta = std::abs(leftPosition.row - rightPosition.row);
    const int columnDelta = std::abs(leftPosition.column - rightPosition.column);
    return rowDelta == 0 ? columnDelta == 1 : rowDelta == 1 && columnDelta <= 1;
}

bool IsQwertyNeighborVariant(std::wstring_view input,
                             std::wstring_view target) noexcept {
    if (input.size() != target.size()) return false;
    bool foundDifference = false;
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == target[index]) continue;
        if (foundDifference || !IsQwertyNeighbor(input[index], target[index])) return false;
        foundDifference = true;
    }
    return foundDifference;
}

}  // namespace

TypoEvidence TypoModel::Compare(std::wstring_view input,
                                std::wstring_view target) const noexcept {
    return {EditDistance(input, target),
            IsRepeatedKeyVariant(input, target),
            IsTranspositionVariant(input, target),
            IsOneInsertedCharacterVariant(target, input),
            IsOneInsertedCharacterVariant(input, target),
            IsQwertyNeighborVariant(input, target)};
}

double TypoModel::WeightedCost(std::wstring_view input, std::wstring_view target) const noexcept {
    return WeightedEditDistance(input, target);
}

bool AreQwertyNeighbors(wchar_t left, wchar_t right) noexcept {
    return IsQwertyNeighbor(left, right);
}

}  // namespace tekito
