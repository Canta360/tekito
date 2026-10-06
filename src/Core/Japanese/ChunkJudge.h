#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tekito {
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
               const IFrequencyProvider& english, const Loanwords* names, Thresholds thresholds = {}) noexcept
        : table_(table), converter_(converter), slips_(slips), english_(english), names_(names),
          thresholds_(thresholds) {}

    struct Judgement {
        bool english{false};
        // The English as it is written (GitHub, API), or the keys.
        std::wstring written;
        // It reads as either language: the other one is as likely.
        bool either{false};
    };
    // `keys`: lower-case letters. `englishBefore`: English chunks right
    // before this one.
    [[nodiscard]] Judgement Judge(std::wstring_view keys, std::size_t englishBefore) const;

private:
    const RomajiTable& table_;
    const JapaneseConverter& converter_;
    const KeyConverter* slips_;
    const IFrequencyProvider& english_;
    const Loanwords* names_;
    Thresholds thresholds_;
};

}  // namespace tekito::japanese
