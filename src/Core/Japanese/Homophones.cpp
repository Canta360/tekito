#include "Core/Japanese/Homophones.h"

#include <charconv>

namespace tekito::japanese {

bool Homophones::Open(const std::filesystem::path& packDirectory) noexcept {
    return table_.Open(packDirectory / L"contexts.tsv");
}

std::optional<double> Homophones::Weight(std::wstring_view word, std::wstring_view context) const {
    if (!IsOpen() || word.empty() || context.empty()) return std::nullopt;
    std::optional<double> weight;
    table_.ForEachRow(ToUtf8(word) + " " + ToUtf8(context), [&](const std::vector<std::string_view>& fields) {
        double value = 0;
        if (fields.size() >= 2) {
            std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), value);
            weight = value;
        }
        return false;
    });
    return weight;
}

}  // namespace tekito::japanese
