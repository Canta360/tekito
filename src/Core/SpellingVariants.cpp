#include "Core/SpellingVariants.h"

#include "Core/ExternalLexiconProvider.h"

#include <fstream>
#include <string>

namespace tekito {
namespace {

std::wstring Widen(std::string_view ascii) {
    // The pack is ASCII letters, apostrophes and hyphens.
    return {ascii.begin(), ascii.end()};
}

}  // namespace

const SpellingVariants& SpellingVariants::Installed() {
    static const SpellingVariants installed = [] {
        SpellingVariants loaded;
        loaded.Load(ExternalLexiconProvider::PackDirectory(L"spelling-variants") / L"variants.tsv");
        return loaded;
    }();
    return installed;
}

bool SpellingVariants::Load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) continue;
        Add(Widen(std::string_view(line).substr(0, tab)), Widen(std::string_view(line).substr(tab + 1)));
    }
    return !Empty();
}

void SpellingVariants::Add(std::wstring american, std::wstring british) {
    american_.emplace(british, american);
    british_.emplace(std::move(american), std::move(british));
}

std::optional<std::wstring> SpellingVariants::InVariety(std::wstring_view lower, EnglishVariety variety) const {
    const auto& other = variety == EnglishVariety::American ? american_ : british_;
    if (variety == EnglishVariety::Either) return std::nullopt;
    const auto found = other.find(std::wstring(lower));
    if (found == other.end()) return std::nullopt;
    return found->second;
}

}  // namespace tekito
