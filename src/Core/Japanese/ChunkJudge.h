#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {
class EnglishSpelling;
class IFrequencyProvider;
}

namespace tekito::japanese {

class JapaneseConverter;
class KeyConverter;
class Loanwords;
class RomajiTable;

// Mixed typing: Japanese and English typed in chunks, a Space after each.
// Judges whether a chunk of keys is English or Japanese.
//
// English: keys that make no kana and are an English word, or that no slip
// makes Japanese (chatgpt); keys using letters romaji keeps for small kana
// (l, x, v, q, c) that are a known word; a well-known name (API, GitHub);
// and a common English word that reads as Japanese only at a high cost.
// A chunk that reads both ways is English in English (two English chunks
// before it); in Japanese, English only when Japanese text writes it in
// Latin letters often (API, but not take). "push site" is Japanese.
class ChunkJudge final {
public:
    struct Thresholds {
        // log10 scores of the frequency pack.
        double commonWord{4.0};
        double knownWord{2.5};
        double listedWord{0.9};
        // The Japanese reading's cost per kana, in the connection matrix's
        // units, from which a common English word is taken as English.
        std::int64_t costPerKana{2300};
        // A name of the japanese-english pack this well known (log2 of
        // Wikipedia editions) is English even when it reads as romaji.
        double knownName{6.5};
        // A word written in Latin letters in Japanese text this often (log10
        // per million sentences) is English in Japanese too (API, Amazon);
        // one seen less (take, same) is Japanese there.
        double inJapaneseText{1.6};
    };

    ChunkJudge(const RomajiTable& table, const JapaneseConverter& converter, const KeyConverter* slips,
               const IFrequencyProvider& english, const Loanwords* names, Thresholds thresholds) noexcept
        : table_(table), converter_(converter), slips_(slips), english_(english), names_(names),
          thresholds_(thresholds) {}
    // With the default thresholds. (A separate constructor, not "= {}":
    // GCC and Clang cannot use a nested struct's member initializers in a
    // default argument of the class that holds it.)
    ChunkJudge(const RomajiTable& table, const JapaneseConverter& converter, const KeyConverter* slips,
               const IFrequencyProvider& english, const Loanwords* names) noexcept
        : ChunkJudge(table, converter, slips, english, names, Thresholds{}) {}

    // `keys` may be the start of a common English word (meetin: meeting), so
    // Mix's live conversion leaves them as typed.
    [[nodiscard]] bool MayStartEnglishWord(std::wstring_view keys) const noexcept;
    struct Judgement {
        bool english{false};
        // The English as it is written (GitHub, API), or the keys.
        std::wstring written;
        // It reads as either language: the other one is as likely.
        bool either{false};
        // A known English word or name (not just keys that make no kana).
        bool word{false};
        // Japanese: the keys as meant, when they are a slip of a particle
        // (dr: de), too short for slip correction.
        std::wstring meant;
    };
    // `keys`: lower-case letters. `englishBefore`: English chunks right
    // before this one. `japaneseInSentence`: the sentence so far has
    // Japanese in it; without, English chunks before make it an English
    // sentence (do you, nice to). `spelling`: English Auto's spelling, to
    // take keys that are no word as the English word with a slip in it
    // (rveiew: review), with `before` the English typed before them;
    // keys a slip correction makes Japanese of stay Japanese.
    [[nodiscard]] Judgement Judge(std::wstring_view keys, std::size_t englishBefore,
                                  bool japaneseInSentence = true, const EnglishSpelling* spelling = nullptr,
                                  std::wstring_view before = {}) const;

    // Japanese with English words typed into it without spaces
    // (reflectsareta: reflect + sareta, kyouhameetinggaarimasu: kyouha +
    // meeting + gaarimasu): the pieces, when the keys do not read as
    // natural Japanese and the pieces do. Only English spelled right (a
    // known word or name) is taken; one or two of them.
    struct Piece {
        bool english{false};
        std::wstring keys;
        // English: as written (GitHub); Japanese: the kana.
        std::wstring text;
    };
    [[nodiscard]] std::optional<std::vector<Piece>> Split(std::wstring_view keys) const;
    // Letters romaji typing hardly uses (l, q, x, v, c without h, and with
    // `lastConsonant` a word ending in a consonant other than n): `word` is
    // English, not kana.
    [[nodiscard]] static bool ForeignLetters(std::wstring_view word, bool lastConsonant = true) noexcept;

    // A whole sentence typed in chunks (keys without the punctuation after
    // them), judged as a whole: first left to right as typed, then each
    // chunk again with the English and Japanese on both sides of it (my
    // name is tanaka, ここに急に English wo).
    [[nodiscard]] std::vector<Judgement> JudgeSentence(const std::vector<std::wstring>& chunks,
                                                       const EnglishSpelling* spelling = nullptr) const;

private:
    // How English writes `word` (GitHub, Google, nice).
    [[nodiscard]] std::wstring WrittenAs(std::wstring_view word) const;
    // An English word run into Japanese after it, starting with a particle
    // (pcwotukau: pcを使う); not when the keys are an English word.
    [[nodiscard]] bool EnglishThenJapanese(std::wstring_view keys) const;

    const RomajiTable& table_;
    const JapaneseConverter& converter_;
    const KeyConverter* slips_;
    const IFrequencyProvider& english_;
    const Loanwords* names_;
    Thresholds thresholds_;
};

}  // namespace tekito::japanese
