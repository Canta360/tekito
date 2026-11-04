#include "Core/Japanese/Compounds.h"

#include <charconv>

namespace tekito::japanese {

bool Compounds::Open(const std::filesystem::path& packDirectory) noexcept {
    return table_.Open(packDirectory / L"entries.tsv");
}

Compounds::Counts Compounds::Find(std::wstring_view kanji, std::wstring_view reading) const {
    Counts counts;
    if (!IsOpen() || kanji.empty()) return counts;
    const std::string wanted = ToUtf8(reading);
    table_.ForEachRow(ToUtf8(kanji), [&](const std::vector<std::string_view>& fields) {
        if (fields.size() < 4 || fields[1] != wanted) return true;
        std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), counts.ends);
        std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), counts.starts);
        return false;
    });
    return counts;
}

}  // namespace tekito::japanese
