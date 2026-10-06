#pragma once

// English Auto on real, correctly typed text: each word of the sentences is
// typed and ended with Space as TEKITO sees it (the text before it as
// context), and Space decides as InputStateMachine does. Every word it
// rewrites is a false correction, since the text was typed as meant.
//
// Run three ways over the same sentences:
// - as TEKITO is, with learning: a rewrite is undone (Backspace) and the
//   word kept, and TEKITO records what the text service records;
// - keeping the word once undone: added to the user dictionary as "keep as
//   typed", as registering it from the list would;
// - deciding at the next word, with it as the text that follows.
// Each also types one real word a letter away per sentence (the -> them),
// to see how many of those Space still puts right. Then the words kept are
// checked against the typo rows of the eval corpus, so keeping them does
// not stop real corrections.

#include <cstddef>
#include <filesystem>
#include <string>

struct EnglishStudyOptions {
    // Leipzig's *-sentences.txt: id, tab, sentence.
    std::filesystem::path sentences;
    // eval_corpus.tsv, for the typo rows (optional).
    std::filesystem::path corpus;
    std::size_t maxSentences{20000};
    std::size_t showExamples{30};
    // One sentence: prints what Space does to each word, and why.
    std::string probe;
    // Only the time per key, with each part of the engine turned off in turn.
    bool latency{false};
    // Sentences as typed, maybe with slips: id, tab, sentence. Each word is
    // typed and ended with Space, after what Space left for the words
    // before it; written to `writeWords` as id, word number, typed word,
    // what Space left and the first nine candidates (split by U+001F), to
    // compare with the same sentences typed right (eval/typo-study.py).
    std::filesystem::path typed;
    std::filesystem::path writeWords;
    // With --typed: each word's Space looks at the word before it again
    // (IConversionEngine::Recheck), as the text service may.
    bool nextWord{false};
};

int RunEnglishStudy(const EnglishStudyOptions& options);
