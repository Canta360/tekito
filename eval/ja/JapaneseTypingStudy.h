#pragma once

// Ways of typing borrowed from SKK, measured before any is offered:
//
// - hints: the typist presses Space where a phrase ends, as SKK marks where
//   a word starts. Today that Space converts and the next key commits what
//   was typed so far; as a hint it would only mark a phrase break, and the
//   whole sentence is converted at the end with the breaks kept.
// - live: live conversion, the text converted on every key (Space would
//   only pick alternatives), with the phrases more than a few back settled
//   so long input stops rewriting itself.
// - register: how often no candidate fits what was meant, whether it could
//   be put together from shorter phrases (SKK's inline registration), and
//   what registering it does to the conversions that follow.
//
// Where the phrases break is taken from the acceptable text itself: a
// phrase starts where kanji or katakana follow hiragana (with a leading
// お or ご), except inside a word the dictionary has written that way
// (戻り値, 立ち消え), as a typist marking words would put it.

#include <cstddef>
#include <filesystem>
#include <string>

namespace tekito::japanese {
class JapaneseConverter;
class JapaneseDictionary;
}  // namespace tekito::japanese

struct TypingStudyOptions {
    std::string study;
    // The japanese-core pack, for its pos.tsv.
    std::filesystem::path pack;
    // Consecutive sentences typed as one input (live only), for long input.
    std::size_t join{1};
    // At most this many rows, evenly spread (0: all).
    std::size_t sample{0};
    std::size_t showExamples{0};
};

int RunTypingStudy(const tekito::japanese::JapaneseDictionary& dictionary,
                   const tekito::japanese::JapaneseConverter& converter, const std::filesystem::path& corpus,
                   const std::filesystem::path& romaji, const TypingStudyOptions& options);
