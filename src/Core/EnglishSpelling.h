#pragma once

#include "Core/SpellingVariants.h"

#include <string>
#include <string_view>
#include <vector>

namespace tekito {

class IConversionEngine;

// English typed among Japanese (Mix), spelled as English Auto spells it
// when Space ends the word: rveiew is review there too.
class EnglishSpelling final {
public:
    explicit EnglishSpelling(const IConversionEngine& engine) noexcept : engine_(engine) {}

    struct Result {
        // What Space leaves: the correction, or the word as typed.
        std::wstring word;
        // Space put a slip right.
        bool corrected{false};
        // The engine's other choices, best first.
        std::vector<std::wstring> others;
    };
    // `typed`: lower-case letters. `before`: the English typed just before
    // it in the sentence, as context.
    [[nodiscard]] Result Spell(std::wstring_view typed, std::wstring_view before) const;
    // American or British, as English Auto spells (UserSettings::englishSpelling).
    void SetVariety(EnglishVariety variety) noexcept { variety_ = variety; }

private:
    const IConversionEngine& engine_;
    EnglishVariety variety_{EnglishVariety::Either};
};

}  // namespace tekito
