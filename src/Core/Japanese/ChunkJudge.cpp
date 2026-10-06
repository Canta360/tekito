#include "Core/Japanese/ChunkJudge.h"

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/RankingData.h"

#include <algorithm>
#include <cwctype>

namespace tekito::japanese {

ChunkJudge::Judgement ChunkJudge::Judge(std::wstring_view keys, std::size_t englishBefore) const {
    Judgement judgement;
    judgement.written = std::wstring(keys);
    const bool letters = !keys.empty() && std::all_of(keys.begin(), keys.end(), [](wchar_t c) {
        return c >= L'a' && c <= L'z';
    });
    if (!letters) return judgement;

    // How the keys read as romaji.
    std::wstring kana;
    bool readable = true;
    for (const auto& token : JapaneseComposer::ParseRomaji(table_, keys)) {
        readable = readable && !token.leftover;
        kana += token.kana;
    }
    // Letters romaji uses only for small kana and ヴ (la, xtu, vi).
    bool rareLetters = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const wchar_t c = keys[i];
        rareLetters = rareLetters || c == L'l' || c == L'q' || c == L'x' || c == L'v' ||
                      (c == L'c' && (i + 1 == keys.size() || keys[i + 1] != L'h'));
    }
    const double score = english_.Score(keys);
    // A well-known name, and how it is written (github: GitHub).
    bool name = false;
    if (names_) {
        for (const auto& spelling : names_->ForEnglish(keys, 1)) {
            // Written with capitals inside (GitHub, API, iPhone), or very
            // well known (Google). Wikipedia capitalizes every title, so
            // Mail stays mail.
            const bool capitals = std::any_of(spelling.english.begin() + 1, spelling.english.end(),
                                              [](wchar_t c) { return std::iswupper(c) != 0; });
            name = capitals || spelling.score >= thresholds_.knownName;
            if (name) judgement.written = spelling.english;
        }
    }

    // Two letters or fewer are particles (to, de, na), not English.
    if (keys.size() <= 2) return judgement;
    if (!readable) {
        // Japanese with a slip in it, unless it is an English word.
        const bool slip = score < thresholds_.listedWord && slips_ && slips_->Convert(keys).has_value();
        judgement.english = !slip;
        return judgement;
    }
    if (rareLetters && score >= thresholds_.knownWord) {
        judgement.english = true;
        return judgement;
    }
    const auto best = converter_.Best(kana);
    const bool japaneseOdd = !best || kana.empty() ||
                             best->cost >= thresholds_.costPerKana * static_cast<std::int64_t>(kana.size());
    // Whether Japanese text writes it in Latin letters (API, Amazon), or
    // hardly ever (take, same). Without the counts, as if it does.
    const auto inJapanese = names_ ? names_->InJapaneseText(keys) : std::nullopt;
    const bool writtenInJapanese = !names_ || !names_->HasJapaneseTextCounts() ||
                                   (inJapanese && *inJapanese >= thresholds_.inJapaneseText);
    if (japaneseOdd && (name || (score >= thresholds_.commonWord && writtenInJapanese))) {
        judgement.english = true;
        return judgement;
    }
    // Reads both ways (kara, take, amazon): as the sentence goes.
    constexpr std::size_t kEnglishSentence = 2;
    if (name || score >= thresholds_.knownWord) {
        judgement.either = true;
        judgement.english = englishBefore >= kEnglishSentence ||
                            (inJapanese && *inJapanese >= thresholds_.inJapaneseText);
    }
    if (!judgement.english) judgement.written = std::wstring(keys);
    return judgement;
}

}  // namespace tekito::japanese
