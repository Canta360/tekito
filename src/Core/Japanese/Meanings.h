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

// The language a meaning is written in.
enum class MeaningLanguage {
    // The word's own: English for an English word, Japanese otherwise.
    Word,
    // Japanese: an English word explained in Japanese.
    Japanese,
    // English: a Japanese word explained in English.
    English,
};

// Japanese slang, Wiktionary and WordNet meanings, English meanings from the
// dictionary-display pack, and WordNet meanings across the two languages
// (english-meanings-ja, japanese-meanings-en). Any pack may be missing.
class MeaningDictionary final {
public:
    // `dataRoot` is the folder with the Data Packs.
    bool Open(const std::filesystem::path& dataRoot) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept {
        return slang_.IsOpen() || wiktionary_.IsOpen() || wordnet_.IsOpen() || english_.IsOpen() ||
               englishInJapanese_.IsOpen() || japaneseInEnglish_.IsOpen();
    }

    // The meaning of a candidate: English for Latin letters, Japanese
    // otherwise. `reading` is the candidate's hiragana reading, when known;
    // it picks between words written alike (私: わたし, あたい). With a
    // `language` other than the word's, the meaning is in that language when
    // one is known, else in the word's own.
    [[nodiscard]] std::optional<Meaning> Lookup(std::wstring_view text, std::wstring_view reading = {},
                                                MeaningLanguage language = MeaningLanguage::Word) const;

private:
    [[nodiscard]] std::optional<Meaning> Japanese(const SortedTsv& source, std::wstring_view text,
                                                  std::wstring_view reading) const;
    [[nodiscard]] std::optional<Meaning> English(std::wstring_view word) const;

    SortedTsv slang_;
    SortedTsv wiktionary_;
    SortedTsv wordnet_;
    SortedTsv english_;
    SortedTsv englishInJapanese_;
    SortedTsv japaneseInEnglish_;
};

}  // namespace tekito::japanese
