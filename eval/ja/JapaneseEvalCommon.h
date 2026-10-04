#pragma once

// What tekito_ja_eval's parts share: the corpus rows, text conversion and
// the measures.

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ja_eval {

inline std::wstring Widen(std::string_view utf8) {
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

inline std::string Narrow(std::wstring_view text) {
#if defined(_WIN32)
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
#else
    return std::string(text.begin(), text.end());
#endif
}

inline std::size_t EditDistance(std::wstring_view a, std::wstring_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

struct Row {
    std::string source;
    std::string id;
    std::wstring reading;
    std::vector<std::wstring> expected;
};

inline std::vector<Row> LoadCorpus(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<Row> rows;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
        // Readings files: source, id, context, reading, expected...
        // Keys files: source, id, keys, expected...
        const bool keys = path.filename().string().find("_keys") != std::string::npos;
        const std::size_t text = keys ? 2 : 3;
        if (fields.size() < text + 2) continue;
        Row row{fields[0], fields[1], Widen(fields[text]), {}};
        for (std::size_t i = text + 1; i < fields.size(); ++i) row.expected.push_back(Widen(fields[i]));
        rows.push_back(std::move(row));
    }
    return rows;
}

inline double Percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(q * values.size()))];
}

// The closest acceptable text: its edit distance from `output` and length.
inline std::pair<std::size_t, std::size_t> Closest(std::wstring_view output,
                                                   const std::vector<std::wstring>& expected) {
    std::size_t distance = SIZE_MAX, length = 0;
    for (const auto& text : expected) {
        const std::size_t d = EditDistance(output, text);
        if (d < distance) {
            distance = d;
            length = text.size();
        }
    }
    return {distance, length};
}

}  // namespace ja_eval
