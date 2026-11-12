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
        const std::wstring reading = FromUtf8(std::string_view(line).substr(0, tab));
        pairs_.insert(reading + L'\t' + FromUtf8(std::string_view(line).substr(tab + 1)));
        readings_.insert(reading);
    }
    return !pairs_.empty();
}

bool UnusualReadings::HasReading(std::wstring_view reading) const {
    return readings_.contains(std::wstring(reading));
}

bool UnusualReadings::Contains(std::wstring_view reading, std::wstring_view word) const {
    std::wstring key(reading);
    key += L'\t';
    key += word;
    return pairs_.contains(key);
}

}  // namespace tekito::japanese
