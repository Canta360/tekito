#include "Core/Japanese/Collocations.h"

#include <charconv>
#include <cmath>

namespace tekito::japanese {

bool Collocations::Open(const std::filesystem::path& packDirectory) noexcept {
    if (!table_.Open(packDirectory / L"pairs.tsv")) return false;
    total_ = Count("~~");
    return total_ > 0;
}

double Collocations::Count(const std::string& key) const {
    double count = 0;
    table_.ForEachRow(key, [&](const std::vector<std::string_view>& fields) {
        if (fields.size() >= 2) std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), count);
        return false;
    });
    return count;
}

std::optional<double> Collocations::Fit(std::wstring_view before, wchar_t particle, std::wstring_view after) const {
    if (!IsOpen() || before.empty() || after.empty()) return std::nullopt;
    const std::string left = ToUtf8(before) + " " + ToUtf8(std::wstring_view(&particle, 1));
    const std::string right = ToUtf8(after);
    const double leftCount = Count(left);
    const double rightCount = Count("~ " + right);
    if (leftCount <= 0 || rightCount <= 0) return std::nullopt;
    const double seen = Count(left + " " + right);
    const double expected = leftCount * rightCount / total_;
    if (seen > 0) return std::log(seen / expected);
    // Never seen: evidence against it only when it would have been seen.
    // (Pairs seen less than the pack keeps count as about once.)
    const double unseen = std::log(1.0 / expected);
    if (unseen >= 0) return std::nullopt;
    return unseen;
}

}  // namespace tekito::japanese
