#include "Core/Japanese/EnglishTies.h"

#include <algorithm>
#include <charconv>

namespace tekito::japanese {

namespace {

// The first senses only: later ones are rarer readings of the word.
constexpr std::size_t kSenses = 4;
constexpr std::size_t kNames = 10;

bool IsLatin(wchar_t c) noexcept { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

}  // namespace

bool EnglishTies::Open(const std::filesystem::path& pairsPack, const std::filesystem::path& meaningsPack) noexcept {
    const bool pairs = pairs_.Open(pairsPack / L"pairs.tsv");
    const bool meanings = meanings_.Open(meaningsPack / L"meanings.tsv");
    return pairs && meanings;
}

std::vector<std::string> EnglishTies::Senses(std::wstring_view word) const {
    std::vector<std::string> names;
    meanings_.ForEachRow(ToUtf8(word), [&](const std::vector<std::string_view>& fields) {
        // word, reading, then one sense a field: "bring around, cure, heal: provide a cure".
        for (std::size_t f = 2; f < fields.size() && f < 2 + kSenses; ++f) {
            const auto sense = fields[f].substr(0, fields[f].find(':'));
            for (std::size_t at = 0; at <= sense.size();) {
                std::size_t end = sense.find(',', at);
                if (end == std::string_view::npos) end = sense.size();
                auto name = sense.substr(at, end - at);
                while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
                while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
                if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c >= 'a' && c <= 'z'; }) &&
                    std::find(names.begin(), names.end(), name) == names.end() && names.size() < kNames) {
                    names.emplace_back(name);
                }
                at = end + 1;
            }
        }
        return false;
    });
    return names;
}

std::vector<std::string> EnglishTies::Meanings(std::wstring_view word) const {
    if (!IsOpen() || word.empty()) return {};
    if (std::all_of(word.begin(), word.end(), IsLatin)) {
        std::string lower;
        for (const wchar_t c : word) lower.push_back(static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
        std::vector<std::string> forms{lower};
        if (lower.size() > 3 && lower.back() == 's' && lower[lower.size() - 2] != 's') forms.push_back(lower.substr(0, lower.size() - 1));
        return forms;
    }
    if (auto names = Senses(word); !names.empty()) return names;
    // A conjugated word (直し, 治して): the dictionary form of its stem.
    std::size_t stem = 0;
    while (stem < word.size() && ((word[stem] >= 0x4E00 && word[stem] <= 0x9FFF) || word[stem] == 0x3005)) ++stem;
    if (stem == 0) return {};
    for (std::size_t kana = std::min(word.size(), stem + 1); kana >= stem; --kana) {
        for (const wchar_t* ending : {L"る", L"す", L"う", L"く", L"つ", L"む", L"ぶ", L"ぐ", L"ぬ", L"い"}) {
            if (auto names = Senses(std::wstring(word.substr(0, kana)) + ending); !names.empty()) return names;
        }
        if (kana == stem) break;
    }
    return {};
}

double EnglishTies::Tie(const std::vector<std::string>& first, const std::vector<std::string>& second) const {
    double strongest = 0.0;
    for (const auto& a : first) {
        for (const auto& b : second) {
            if (a == b) continue;
            const std::string key = a < b ? a + " " + b : b + " " + a;
            pairs_.ForEachRow(key, [&](const std::vector<std::string_view>& fields) {
                double pmi = 0.0;
                if (fields.size() >= 2) std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), pmi);
                strongest = std::max(strongest, pmi);
                return false;
            });
        }
    }
    return strongest;
}

}  // namespace tekito::japanese
