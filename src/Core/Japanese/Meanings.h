#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// What a word means, for the candidate window.
struct Meaning {
    // The word the meaning is for: the dictionary form when the candidate
    // is inflected ("会った" -> 会う).
    std::wstring headword;
    std::vector<std::wstring> senses;
};

// Word meanings from the japanese-wiktionary and japanese-wordnet packs
// (Wiktionary first) and English meanings from the dictionary-display pack.
// Any of them may be missing.
class MeaningDictionary final {
public:
    // `dataRoot` is the folder with the Data Packs.
    bool Open(const std::filesystem::path& dataRoot) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept {
        return wiktionary_.IsOpen() || wordnet_.IsOpen() || english_.IsOpen();
    }

    // The meaning of a candidate: English for Latin letters, Japanese
    // otherwise. `reading` is the candidate's hiragana reading, when known;
    // it picks between words written alike (私: わたし, あたい).
    [[nodiscard]] std::optional<Meaning> Lookup(std::wstring_view text, std::wstring_view reading = {}) const;

private:
    [[nodiscard]] std::optional<Meaning> Japanese(const SortedTsv& source, std::wstring_view text,
                                                  std::wstring_view reading) const;
    [[nodiscard]] std::optional<Meaning> English(std::wstring_view word) const;

    SortedTsv wiktionary_;
    SortedTsv wordnet_;
    SortedTsv english_;
};

}  // namespace tekito::japanese
