#include "Core/Japanese/UnusualReadings.h"

#include "Core/Japanese/SortedTsv.h"

#include <fstream>

namespace tekito::japanese {

bool UnusualReadings::Open(const std::filesystem::path& file) {
    readings_.clear();
    pairs_.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos || tab == 0 || tab + 1 == line.size()) continue;
        const auto kindTab = line.find('\t', tab + 1);
        const std::string_view word = std::string_view(line).substr(tab + 1, kindTab == std::string::npos ? std::string::npos : kindTab - tab - 1);
        const bool archaic = kindTab != std::string::npos && std::string_view(line).substr(kindTab + 1) == "archaic";
        const std::wstring reading = FromUtf8(std::string_view(line).substr(0, tab));
        pairs_[reading + L'\t' + FromUtf8(word)] = archaic ? Kind::Archaic : Kind::Rare;
        readings_.insert(reading);
    }
    return !pairs_.empty();
}

bool UnusualReadings::HasReading(std::wstring_view reading) const {
    return readings_.contains(std::wstring(reading));
}

std::optional<UnusualReadings::Kind> UnusualReadings::Find(std::wstring_view reading, std::wstring_view word) const {
    std::wstring key(reading);
    key += L'\t';
    key += word;
    const auto found = pairs_.find(key);
    if (found == pairs_.end()) return std::nullopt;
    return found->second;
}

}  // namespace tekito::japanese
