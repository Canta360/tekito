#pragma once

#include "Core/Japanese/MappedFile.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// A TSV file sorted bytewise by its first field, mapped and searched in
// place: nothing is read or indexed up front, so opening it in every
// application costs nothing and a lookup touches a few pages.
class SortedTsv final {
public:
    bool Open(const std::filesystem::path& path) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return file_.Size() > 0; }
    // Calls `row` with the fields of every row whose first field is `key`,
    // in file order, until it returns false.
    void ForEachRow(std::string_view key,
                    const std::function<bool(const std::vector<std::string_view>& fields)>& row) const;

private:
    [[nodiscard]] std::size_t LineStart(std::size_t at) const noexcept;
    [[nodiscard]] std::string_view KeyAt(std::size_t lineStart) const noexcept;

    MappedFile file_;
};

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
