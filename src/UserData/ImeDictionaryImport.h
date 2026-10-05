#pragma once

#include "Core/Japanese/JapaneseUserDictionary.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace tekito::userdata {

// Words from another input method's user dictionary, exported as text:
// Microsoft IME (UTF-16, "!Microsoft IME Dictionary Tool"), Google Japanese
// Input and ATOK. Each line is "reading <TAB> word <TAB> part of speech"
// (a comment may follow); lines starting with "!" or "#" are headers.
struct ImportedImeWords {
    std::vector<japanese::UserWord> words;
    // Lines that held no word TEKITO can use (a reading that is not kana,
    // say).
    std::size_t skipped{0};
};

// The words in the file's text (UTF-16 with a byte order mark, UTF-8, or
// Shift_JIS); nothing if no line is a word.
[[nodiscard]] std::optional<ImportedImeWords> ParseImeDictionary(std::string_view bytes);
[[nodiscard]] std::optional<ImportedImeWords> ReadImeDictionary(const std::filesystem::path& file);

// `imported` added to `words`, skipping words already there; how many were added.
std::size_t MergeImeWords(std::vector<japanese::UserWord>& words, const std::vector<japanese::UserWord>& imported);

}  // namespace tekito::userdata
