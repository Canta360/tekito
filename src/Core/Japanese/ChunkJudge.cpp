#include "Core/Japanese/ChunkJudge.h"

#include "Core/EnglishSpelling.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LetterOdds.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/WrittenForms.h"
#include "Core/RankingData.h"
#include "Core/RunTogether.h"
#include "Core/TypoModel.h"

#include <algorithm>
#include <cwctype>
#include <map>

namespace tekito::japanese {

bool ChunkJudge::MayStartEnglishWord(std::wstring_view keys) const noexcept {
    constexpr double kCommon = 3.5;
    return english_.CommonWordStartsWith(keys, kCommon);
}

namespace {

// The particles that follow an English noun (Stack Overflowで, GitHubに).
constexpr std::wstring_view kParticles[] = {L"de", L"ga", L"ni", L"wo", L"ha", L"wa", L"no", L"mo", L"ka",
                                            L"ya", L"ne", L"yo", L"na", L"da", L"e", L"he", L"te"};

// The particle two keys are one slip from: a neighboring key, or the two
// swapped (dr: de, ow: wo).
std::wstring_view SlipOfParticle(std::wstring_view keys) {
    if (keys.size() != 2) return {};
    for (const auto particle : kParticles) {
        if (particle.size() != 2) continue;
        const bool swapped = keys[0] == particle[1] && keys[1] == particle[0];
        const bool first = keys[0] == particle[0] && AreQwertyNeighbors(keys[1], particle[1]);
        const bool second = keys[1] == particle[1] && AreQwertyNeighbors(keys[0], particle[0]);
        if (swapped || first || second) return particle;
    }
    return {};
}

}  // namespace

std::wstring ChunkJudge::WordAsWritten(std::wstring_view word, bool inJapanese) const {
    // A name of its own (OK, GitHub); an acronym a name list has for a
    // common word only when written so (is, we, now: not IS, WE, NOW).
    if (forms_) {
        if (const auto form = forms_->Find(word); form && (inJapanese || !form->japaneseOnly)) return form->written;
    }
    const std::wstring named = WrittenAs(word, inJapanese);
    const bool capitals = std::none_of(named.begin(), named.end(), [](wchar_t c) { return std::iswlower(c) != 0; });
    if (capitals && english_.Score(word) >= thresholds_.knownWord) return std::wstring(word);
    return named;
}

std::wstring ChunkJudge::WrittenAs(std::wstring_view word, bool inJapanese) const {
    std::wstring lower(word);
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    if (forms_) {
        if (const auto form = forms_->Find(lower); form && (inJapanese || !form->japaneseOnly)) return form->written;
    }
    if (names_) {
        if (const auto written = names_->WrittenAs(lower)) return *written;
        for (const auto& spelling : names_->ForEnglish(lower, 1)) {
            const bool capitals = std::any_of(spelling.english.begin() + 1, spelling.english.end(),
                                              [](wchar_t c) { return std::iswupper(c) != 0; });
            if (capitals) return spelling.english;
        }
    }
    return AsName(std::wstring(word), inJapanese);
}

std::wstring ChunkJudge::AsName(std::wstring written, bool inJapanese) const {
    if ((!englishNames_ && !productNames_) || written.size() < 2) return written;
    if (std::any_of(written.begin(), written.end(), [](wchar_t c) { return std::iswupper(c) != 0; })) return written;
    // A form of its own written so on purpose (docomo).
    if (forms_ && forms_->Find(written)) return written;
    // A name in english-names with only its first letter a capital
    // (Whisky, Vercel) only when English writes it so too (Kindle) or
    // hardly uses it as a word (vercel; not whisky, 2.97).
    constexpr double kRareWord = 2.5;
    const auto titleOnly = [](const std::wstring& name) {
        return name.size() > 1 && std::iswupper(name.front()) != 0 &&
               std::none_of(name.begin() + 1, name.end(), [](wchar_t c) { return std::iswupper(c) != 0; });
    };
    for (const WrittenForms* names : {englishNames_, productNames_}) {
        if (!names) continue;
        const auto name = names->Find(written);
        if (!name || (!inJapanese && name->japaneseOnly) ||
            std::none_of(name->written.begin(), name->written.end(), [](wchar_t c) { return std::iswupper(c) != 0; })) {
            continue;
        }
        if (names == englishNames_ && titleOnly(name->written) && english_.Score(written) >= kRareWord &&
            !(names_ && names_->WrittenAs(written))) {
            continue;
        }
        return name->written;
    }
    return written;
}

ChunkJudge::Judgement ChunkJudge::Judge(std::wstring_view keys, std::size_t englishBefore,
                                        bool japaneseInSentence, const EnglishSpelling* spelling,
                                        std::wstring_view before, bool englishSentences) const {
    auto judgement = JudgeLanguage(keys, englishBefore, japaneseInSentence, spelling, before, englishSentences);
    // Among Japanese unless English so far makes an English sentence.
    const bool englishSoFar = englishBefore >= 2 || (englishBefore > 0 && !japaneseInSentence);
    if (judgement.english) judgement.written = AsName(std::move(judgement.written), !englishSoFar);
    return judgement;
}

ChunkJudge::Judgement ChunkJudge::JudgeLanguage(std::wstring_view keys, std::size_t englishBefore,
                                                bool japaneseInSentence, const EnglishSpelling* spelling,
                                                std::wstring_view before, bool englishSentences) const {
    Judgement judgement;
    judgement.written = std::wstring(keys);
    // A word with a number after it (iphone15, gpt4): as the word is, with
    // the number kept.
    std::size_t digits = keys.size();
    while (digits > 0 && keys[digits - 1] >= L'0' && keys[digits - 1] <= L'9') --digits;
    if (digits > 0 && digits < keys.size()) {
        auto word = Judge(keys.substr(0, digits), englishBefore, japaneseInSentence, spelling, before, englishSentences);
        word.written += keys.substr(digits);
        return word;
    }
    const bool letters = !keys.empty() && std::all_of(keys.begin(), keys.end(), [](wchar_t c) {
        return c >= L'a' && c <= L'z';
    });
    if (!letters) {
        // A contraction or possessive (don't, we're, tomorrow's): English
        // when it is a word, or the word before the apostrophe is.
        const auto apostrophe = keys.find(L'\'');
        const bool word = apostrophe != std::wstring_view::npos && apostrophe > 0 && apostrophe + 1 < keys.size() &&
                          std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || c == L'\''; });
        if (word && (english_.Score(keys) >= thresholds_.listedWord ||
                     english_.Score(keys.substr(0, apostrophe)) >= thresholds_.knownWord)) {
            judgement.english = true;
            judgement.word = true;
            if (keys.starts_with(L"i'")) judgement.written[0] = L'I';
        }
        return judgement;
    }
    // A first letter typed twice (bbug): no Japanese starts with っ, so the
    // English word, when the rest is one.
    if (keys.size() >= 4 && keys[0] == keys[1] && std::wstring_view(L"aiueon").find(keys[0]) == std::wstring_view::npos) {
        auto rest = Judge(keys.substr(1), englishBefore, japaneseInSentence, spelling, before, englishSentences);
        if (rest.english && rest.word) return rest;
    }

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
    judgement.word = score >= thresholds_.knownWord;
    // A well-known name, and how it is written (github: GitHub).
    bool name = false;
    if (names_) {
        for (const auto& named : names_->ForEnglish(keys, 1)) {
            // Written with capitals inside (GitHub, API, iPhone), or very
            // well known (Google).
            const bool capitals = std::any_of(named.english.begin() + 1, named.english.end(),
                                              [](wchar_t c) { return std::iswupper(c) != 0; });
            name = capitals || named.score >= thresholds_.knownName;
            judgement.word = judgement.word || name;
            // A name English news does not write (Qiita) as the pack does.
            if (capitals && score <= 0.0) judgement.written = named.english;
        }
        // As English writes it: Google, but nice (Nice is a city too) and
        // it (not IT).
        if (const auto written = names_->WrittenAs(keys)) judgement.written = *written;
    }

    // Its own name (ram: RAM, nvidia: NVIDIA), over how a name list or
    // English writes it; a word that is plain English too (line, booth)
    // only outside an English sentence. An acronym of three letters or more
    // so named is English even when it reads as romaji (uefi, not 上フィ).
    if (forms_) {
        const bool englishSoFar = englishBefore >= 2 || (englishBefore > 0 && !japaneseInSentence);
        if (const auto form = forms_->Find(keys); form && (!form->japaneseOnly || !englishSoFar)) {
            judgement.written = form->written;
            judgement.word = true;
            name = true;
            const bool acronym = std::none_of(form->written.begin(), form->written.end(),
                                              [](wchar_t c) { return std::iswlower(c) != 0; });
            if (acronym && keys.size() >= 3 && !form->japaneseOnly) {
                judgement.english = true;
                return judgement;
            }
        }
    }
    // An English sentence typed without spaces (seeyouonmonday,
    // pleasecheckthegithubpage): known words make up all of it, two or
    // more when romaji cannot read it, three or more when it reads as
    // Japanese only unnaturally; a sentence, so with a capital. Not a word
    // the list has (therapist is not the rapist). Japanese that
    // reads naturally stays, and so do English words run into Japanese
    // (meetingde, fixshitanode), which are split.
    constexpr std::size_t kSentenceLetters = 8;
    if (englishSentences && keys.size() >= kSentenceLetters && score < thresholds_.listedWord) {
        if (const auto words = EnglishWords(keys); words && words->size() >= (readable ? 3U : 2U) && !Split(keys)) {
            bool natural = false;
            if (readable && !kana.empty()) {
                const auto cost = converter_.Cost(kana);
                natural = cost && *cost < thresholds_.costPerKana * static_cast<std::int64_t>(kana.size());
            }
            if (!natural) {
                judgement.english = true;
                judgement.word = true;
                judgement.written.clear();
                for (const auto& word : *words) judgement.written += (judgement.written.empty() ? L"" : L" ") + word;
                judgement.written[0] = static_cast<wchar_t>(std::towupper(judgement.written[0]));
                return judgement;
            }
        }
    }
    // "i" alone is I: い alone is hardly ever meant.
    if (keys == L"i") {
        judgement.english = true;
        judgement.written = L"I";
        return judgement;
    }
    // Two letters or fewer that read as kana: Japanese, but English after
    // English (let me, see you on), except the particles that follow an
    // English noun (Stack Overflowで, GitHubに); to (と) only in an English
    // sentence (nice to meet you).
    constexpr std::size_t kEnglishSentence = 2;
    // English so far with no Japanese: an English sentence.
    const bool englishSentence = englishBefore >= kEnglishSentence || (englishBefore > 0 && !japaneseInSentence);
    if (keys.size() <= 2 && readable) {
        const bool particle = std::find(std::begin(kParticles), std::end(kParticles), keys) != std::end(kParticles);
        // English before, or the start of a sentence (do you, so far).
        const bool sentenceStart = englishBefore == 0 && !japaneseInSentence;
        const bool after = keys == L"to" ? englishSentence : englishBefore > 0 || sentenceStart;
        // Or letters Japanese text writes in capitals very often, wherever
        // they are (ai: AI, not 愛; log10 per million sentences 2.58).
        constexpr double kAcronymInJapanese = 2.5;
        const auto seen = names_ ? names_->InJapaneseText(keys) : std::nullopt;
        const auto written = names_ ? names_->WrittenAs(keys) : std::nullopt;
        // Not an everyday Japanese word (ii: いい, not II; ai: 愛 is rarer).
        const auto japanese = converter_.Best(kana);
        const bool everyday = japanese && !kana.empty() &&
                              japanese->cost < thresholds_.everydayPerKana * static_cast<std::int64_t>(kana.size());
        const bool acronym = seen && *seen >= kAcronymInJapanese && written && !everyday &&
                             std::none_of(written->begin(), written->end(), [](wchar_t c) { return std::iswlower(c) != 0; });
        judgement.english = !particle && ((after && score >= thresholds_.commonWord) || acronym);
        if (acronym) judgement.written = *written;
        if (!judgement.english) judgement.written = std::wstring(keys);
        return judgement;
    }
    // Keys that are no word: English with a slip in it when English Auto
    // puts it right to a word Japanese text writes in Latin letters at all
    // (rveiew: review; not delinai: Celina), or in an English sentence.
    const auto spelled = [&]() -> bool {
        if (!spelling) return false;
        const auto result = spelling->Spell(keys, before);
        if (!result.corrected) return false;
        std::wstring lower = result.word;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        const auto seen = names_ ? names_->InJapaneseText(lower) : std::nullopt;
        const bool used = !names_ || !names_->HasJapaneseTextCounts() || englishBefore >= kEnglishSentence ||
                          seen.has_value();
        if (!used) return false;
        judgement.english = true;
        judgement.word = true;
        judgement.written = WrittenAs(result.word);
        return true;
    };
    // English no list has, as its letters go (cappuccino, gibberellin:
    // ln odds 15 and more; Japanese romaji below 0, arimasen 8): five
    // letters or more, and no Japanese word. As English Auto spells it.
    constexpr double kEnglishLetters = 12.0;
    constexpr std::size_t kEnglishLetterKeys = 5;
    // Not English run into Japanese, which is split off (meetingde:
    // meetingで), nor English after a particle (pcwotukau).
    const bool englishLetters = letterOdds_ && keys.size() >= kEnglishLetterKeys &&
                                letterOdds_->Score(keys) >= kEnglishLetters && !converter_.IsWord(kana) &&
                                !EnglishThenJapanese(keys) && !Split(keys);
    if (!readable) {
        // Japanese with a slip in it, unless it is an English word.
        const bool known = score >= thresholds_.listedWord || name;
        // Two letters after English one slip from a particle (dr: で) are
        // the particle, unless written in capitals (PC, OK).
        const bool capitals = std::all_of(judgement.written.begin(), judgement.written.end(),
                                          [](wchar_t c) { return std::iswupper(c) != 0; });
        const bool slip = slips_ && slips_->Convert(keys).has_value();
        if (const auto particle = SlipOfParticle(keys); englishBefore > 0 && !capitals && !particle.empty()) {
            judgement.meant = std::wstring(particle);
            return judgement;
        }
        if (known) {
            judgement.english = true;
            return judgement;
        }
        // English run into Japanese (pcwotukau) is Japanese. An English
        // word with a slip in it (chaannel: channel), as English Auto puts it
        // right to a word Japanese text writes in Latin letters, comes before
        // a slip correction that makes Japanese of it, after English or with
        // letters romaji hardly uses; otherwise, in Japanese, when in doubt
        // Japanese (iktuo: 行くと, not into).
        if (EnglishThenJapanese(keys)) return judgement;
        // Japanese put right comes first (miizugamoreteta: 水が漏れてた);
        // English Auto's spelling only for letters romaji hardly uses
        // (chaannel: channel) or when no slip makes Japanese of it.
        if (ForeignLetters(keys, false) && spelled()) return judgement;
        if (englishLetters) {
            // A name keeps how it is written (FeliCa); other keys as English
            // Auto spells them.
            if (name || !spelled()) judgement.english = true;
            return judgement;
        }
        if (slip) return judgement;
        if (spelled()) return judgement;
        judgement.english = true;
        return judgement;
    }
    if (rareLetters && score >= thresholds_.knownWord) {
        judgement.english = true;
        return judgement;
    }
    // A dictionary word reads naturally, however short (あれ); a loanword
    // the dictionary writes in katakana (アマゾン) is the English itself.
    const auto best = converter_.Best(kana);
    const bool katakana = best && !best->text.empty() && std::all_of(best->text.begin(), best->text.end(), [](wchar_t c) {
        return (c >= L'\x30A1' && c <= L'\x30FA') || c == L'\x30FC' || c == L'\x30FB';
    });
    // Whether Japanese text writes it in Latin letters (API, Amazon), or
    // hardly ever (take, same). Without the counts, as if it does.
    const auto inJapanese = names_ ? names_->InJapaneseText(keys) : std::nullopt;
    const bool writtenInJapanese = !names_ || !names_->HasJapaneseTextCounts() ||
                                   (inJapanese && *inJapanese >= thresholds_.inJapaneseText);
    // A loanword written in katakana just as the romaji spells it (anime:
    // アニメ, yoga: ヨガ) is Japanese's own, unless Japanese text writes it
    // in Latin letters (amazon: Amazon).
    const bool ownKatakana = katakana && !writtenInJapanese && best->text == ToKatakana(kana);
    const bool japaneseWord = (!katakana || ownKatakana) && converter_.IsWord(kana);
    if (englishLetters && !japaneseWord) {
        if (name || !spelled()) judgement.english = true;
        return judgement;
    }
    const bool japaneseOdd = !japaneseWord &&
                             (!best || kana.empty() || katakana ||
                              best->cost >= thresholds_.costPerKana * static_cast<std::int64_t>(kana.size()));
    // Two English chunks or more with English grammar in them (is, the, a)
    // and no Japanese yet: the English sentence goes on in English (my name
    // is tanaka), unless the keys read as natural Japanese of more than one
    // word. English nouns and verbs alone (version up, npm install) are
    // often Japanese's (した).
    constexpr double kGrammarWord = 5.9;
    bool grammar = false;
    for (std::size_t at = 0; at < before.size() && !grammar;) {
        std::size_t end = at;
        while (end < before.size() && std::iswalpha(before[end])) ++end;
        if (end > at) {
            std::wstring word(before.substr(at, end - at));
            for (auto& c : word) c = static_cast<wchar_t>(std::towlower(c));
            grammar = english_.Score(word) >= kGrammarWord;
        }
        at = end + 1;
    }
    if (englishBefore >= kEnglishSentence && !japaneseInSentence && grammar && (japaneseOdd || japaneseWord)) {
        judgement.english = true;
        return judgement;
    }
    // A common English word is English when its romaji is no Japanese
    // word (issue: いっすえ), or when Japanese text writes it in Latin
    // letters; take stays 竹.
    if ((japaneseOdd && name) ||
        (score >= thresholds_.commonWord && (!japaneseWord || (japaneseOdd && writtenInJapanese)))) {
        judgement.english = true;
        return judgement;
    }
    // Reads both ways (kara, take, amazon): as the sentence goes.
    // In an English sentence English (my name, come here); otherwise
    // English when Japanese text writes it so and it is no Japanese word,
    // or when the sentence has other English in it and the romaji is no
    // everyday Japanese (merge to deploy no data: data; push site: して).
    if (name || score >= thresholds_.knownWord) {
        judgement.either = true;
        const bool everyday = best && !kana.empty() &&
                              best->cost < thresholds_.everydayPerKana * static_cast<std::int64_t>(kana.size());
        judgement.english = englishSentence ||
                            (!japaneseWord && inJapanese && *inJapanese >= thresholds_.inJapaneseText);
        // Only a word Japanese text writes in Latin letters at all (not
        // mou: もう).
        if (!judgement.english && englishAround_ && !before.empty() && !everyday && inJapanese) {
            judgement.english = true;
            judgement.byEnglishAround = true;
        }
    }
    if (!judgement.english) judgement.written = std::wstring(keys);
    return judgement;
}

std::optional<std::vector<std::wstring>> ChunkJudge::EnglishWords(std::wstring_view keys) const {
    // Words of three letters or more this well known in English, and
    // shorter ones common (a, to, on).
    constexpr std::size_t kShortWord = 2;
    // Not a particle (no, de): those make it Japanese with English in it.
    // Nor a name or an acronym (Kara, NIH) unless common, but a product
    // written with a capital inside (GitHub).
    const auto known = [&](std::wstring_view word) {
        if (std::find(std::begin(kParticles), std::end(kParticles), word) != std::end(kParticles)) return false;
        const double score = english_.Score(word);
        if (word.size() <= kShortWord || score >= thresholds_.commonWord) return score >= thresholds_.commonWord;
        if (score < thresholds_.knownWord) return false;
        const std::wstring written = WrittenAs(word, false);
        const bool lower = std::none_of(written.begin(), written.end(), [](wchar_t c) { return std::iswupper(c) != 0; });
        const bool inside = std::any_of(written.begin() + 1, written.end(), [](wchar_t c) { return std::iswupper(c) != 0; }) &&
                            std::any_of(written.begin(), written.end(), [](wchar_t c) { return std::iswlower(c) != 0; });
        return lower || inside;
    };
    auto words = SplitRunTogether(keys, known, [&](std::wstring_view word) { return english_.Score(word); });
    if (!words) return std::nullopt;
    // A letter alone other than a and i is no word (s or a hai i teen kid
    // a: Japanese with a slip, not English).
    if (std::any_of(words->begin(), words->end(), [](const std::wstring& w) { return w.size() == 1 && w != L"a" && w != L"i"; })) {
        return std::nullopt;
    }
    for (auto& word : *words) word = WordAsWritten(word, false);
    return words;
}

bool ChunkJudge::ForeignLetters(std::wstring_view word, bool lastConsonant) noexcept {
    for (std::size_t i = 0; i < word.size(); ++i) {
        const wchar_t c = word[i];
        if (c == L'l' || c == L'q' || c == L'x' || c == L'v') return true;
        if (c == L'c' && (i + 1 == word.size() || word[i + 1] != L'h')) return true;
    }
    if (!lastConsonant || word.empty()) return false;
    return std::wstring_view(L"aeioun").find(word.back()) == std::wstring_view::npos;
}

std::optional<std::vector<ChunkJudge::Piece>> ChunkJudge::Split(std::wstring_view keys) const {
    // An English word costs this many kana of ordinary Japanese, a common
    // one or a name written by its own name (page, Figma) this many tenths;
    // shorter than this many letters
    // only names written with capitals (API).
    constexpr std::int64_t kEnglishKana = 2;
    constexpr std::int64_t kCommonEnglishTenths = 15;
    constexpr std::size_t kShortestWord = 4;
    constexpr std::size_t kMostWords = 24;
    constexpr std::size_t kLongestKeys = 80;
    // Letters, and - for ー (to-suto).
    if (keys.size() < kShortestWord + 2 || keys.size() > kLongestKeys ||
        !std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return (c >= L'a' && c <= L'z') || c == L'-'; })) {
        return std::nullopt;
    }
    const auto kanaOf = [&](std::wstring_view part) -> std::optional<std::wstring> {
        std::wstring kana;
        for (const auto& token : JapaneseComposer::ParseRomaji(table_, part)) {
            if (token.leftover) return std::nullopt;
            kana += token.kana;
        }
        return kana;
    };
    // What a run of keys costs as Japanese, nothing when it is not natural.
    std::map<std::pair<std::size_t, std::size_t>, std::optional<std::int64_t>> costs;
    // Two keys or fewer are a particle (githubni) or nothing (plugin: not
    // plug + in).
    constexpr std::size_t kShortPiece = 2;
    const auto japaneseCost = [&](std::size_t begin, std::size_t end) -> std::optional<std::int64_t> {
        if (begin == end) return 0;
        if (end - begin <= kShortPiece &&
            std::find(std::begin(kParticles), std::end(kParticles), keys.substr(begin, end - begin)) == std::end(kParticles)) {
            return std::nullopt;
        }
        const auto key = std::make_pair(begin, end);
        if (const auto found = costs.find(key); found != costs.end()) return found->second;
        std::optional<std::int64_t> cost;
        if (const auto kana = kanaOf(keys.substr(begin, end - begin)); kana && !kana->empty()) {
            const auto found = converter_.Cost(*kana);
            if (found && *found < thresholds_.costPerKana * static_cast<std::int64_t>(kana->size())) cost = found;
        }
        costs.emplace(key, cost);
        return cost;
    };
    // The keys as Japanese, when they read as kana.
    std::optional<std::int64_t> whole;
    if (const auto kana = kanaOf(keys); kana && !kana->empty()) whole = converter_.Cost(*kana);
    // The English words in the keys, longest first.
    struct Word {
        std::size_t begin, end;
    };
    std::vector<Word> words;
    for (std::size_t length = keys.size() - 1; length >= 2; --length) {
        for (std::size_t begin = 0; begin + length <= keys.size() && words.size() < kMostWords; ++begin) {
            const auto word = keys.substr(begin, length);
            // Two letters: only an acronym written by its own name that
            // reads as no romaji (okdesu: OKです, pcwo: PCを; not ai).
            if (length == 2) {
                const auto form = forms_ ? forms_->Find(std::wstring(word)) : std::nullopt;
                if (form && !form->japaneseOnly && !kanaOf(word)) words.push_back({begin, begin + length});
                continue;
            }
            // Letters romaji hardly uses make a rarer word (docker, json)
            // and a shorter one (bug) English enough.
            constexpr double kForeignBy = 0.6;
            const bool foreign = ForeignLetters(word, false);
            bool known = (length >= kShortestWord && english_.Score(word) >= thresholds_.knownWord) ||
                         (foreign && english_.Score(word) >= thresholds_.knownWord - kForeignBy);
            // A name written by its own name (Figma, Qiita, JSON).
            if (!known && forms_ && word.size() >= 3 && forms_->Find(std::wstring(word))) known = true;
            if (!known && names_) {
                for (const auto& spelling : names_->ForEnglish(word, 1)) {
                    const bool capitals = std::any_of(spelling.english.begin() + 1, spelling.english.end(),
                                                      [](wchar_t c) { return std::iswupper(c) != 0; });
                    known = capitals || (length >= kShortestWord && spelling.score >= thresholds_.knownName);
                }
            }
            if (known) words.push_back({begin, begin + length});
        }
    }
    const std::int64_t wordCost = thresholds_.costPerKana * kEnglishKana;
    const auto costOf = [&](const Word& w) {
        const auto word = keys.substr(w.begin, w.end - w.begin);
        const bool common = english_.Score(word) >= thresholds_.commonWord || (forms_ && forms_->Find(std::wstring(word)));
        return common ? thresholds_.costPerKana * kCommonEnglishTenths / 10 : wordCost;
    };
    std::optional<std::int64_t> best;
    std::vector<Word> chosen;
    // One word, or two apart, with natural Japanese around them.
    const auto tryWords = [&](const std::vector<Word>& pick) {
        std::int64_t cost = 0;
        for (const auto& w : pick) cost += costOf(w);
        std::size_t at = 0;
        bool japanese = false;
        for (const auto& word : pick) {
            const auto gap = japaneseCost(at, word.begin);
            if (!gap) return;
            cost += *gap;
            japanese = japanese || word.begin > at;
            at = word.end;
        }
        const auto tail = japaneseCost(at, keys.size());
        if (!tail) return;
        cost += *tail;
        japanese = japanese || at < keys.size();
        if (!japanese || (best && *best <= cost)) return;
        best = cost;
        chosen = pick;
    };
    for (std::size_t a = 0; a < words.size(); ++a) {
        tryWords({words[a]});
        for (std::size_t b = 0; b < words.size(); ++b) {
            if (words[b].begin < words[a].end) continue;
            tryWords({words[a], words[b]});
            for (std::size_t c = 0; c < words.size(); ++c) {
                if (words[c].begin >= words[b].end) tryWords({words[a], words[b], words[c]});
            }
        }
    }
    // Better than the keys as Japanese by this many kana of it.
    constexpr std::int64_t kBetterByKana = 1;
    if (!best || (whole && *best + thresholds_.costPerKana * kBetterByKana >= *whole)) return std::nullopt;
    // Nor than the keys read as Japanese with a slip undone, at a slip's
    // cost (KeyConverter::Costs::typo): Japanese typed with a slip is not
    // English (jinkennypuraibashi-wo is 人権のプライバシーを, not JinKenny).
    constexpr std::int64_t kSlip = 6000;
    if (slips_) {
        if (const auto corrected = slips_->Convert(keys)) {
            if (const auto cost = converter_.Cost(corrected->reading); cost && *best >= *cost + kSlip) {
                return std::nullopt;
            }
        }
    }
    std::vector<Piece> pieces;
    std::size_t at = 0;
    const auto japanese = [&](std::size_t begin, std::size_t end) {
        if (begin < end) pieces.push_back({false, std::wstring(keys.substr(begin, end - begin)), *kanaOf(keys.substr(begin, end - begin))});
    };
    for (const auto& word : chosen) {
        japanese(at, word.begin);
        const auto typed = keys.substr(word.begin, word.end - word.begin);
        pieces.push_back({true, std::wstring(typed), WordAsWritten(typed, true)});
        at = word.end;
    }
    japanese(at, keys.size());
    return pieces;
}

std::vector<ChunkJudge::Judgement> ChunkJudge::JudgeSentence(const std::vector<std::wstring>& chunks,
                                                             const EnglishSpelling* spelling,
                                                             bool englishSentences) const {
    std::vector<Judgement> judged;
    judged.reserve(chunks.size());
    // As typed: left to right.
    std::size_t run = 0;
    bool japanese = false;
    std::wstring englishText;
    for (const auto& keys : chunks) {
        auto judgement = Judge(keys, run, japanese, spelling, englishText, englishSentences);
        if (judgement.english) {
            ++run;
            englishText += judgement.written + L" ";
        } else {
            run = 0;
            japanese = true;
        }
        judged.push_back(std::move(judgement));
    }
    // Again, each with the whole sentence around it, until nothing changes.
    constexpr int kRounds = 3;
    for (int round = 0; round < kRounds; ++round) {
        bool changed = false;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            // English right before and right after it; Japanese anywhere
            // else; the sentence's other English.
            std::size_t around = 0;
            for (std::size_t j = i; j-- > 0 && judged[j].english;) ++around;
            for (std::size_t j = i + 1; j < chunks.size() && judged[j].english; ++j) ++around;
            bool otherJapanese = false;
            std::wstring others;
            for (std::size_t j = 0; j < chunks.size(); ++j) {
                if (j == i) continue;
                if (judged[j].english) {
                    others += judged[j].written + L" ";
                } else {
                    otherJapanese = true;
                }
            }
            // With Japanese in the sentence, English around two letters does
            // not make them English (server to database wo: と); around a
            // word it does (node version up sitai).
            constexpr std::size_t kParticleLength = 2;
            if (otherJapanese && chunks[i].size() <= kParticleLength) around = std::min<std::size_t>(around, 1);
            auto again = Judge(chunks[i], around, otherJapanese, spelling, others, englishSentences);
            if (again.english != judged[i].english) changed = true;
            judged[i] = std::move(again);
        }
        if (!changed) break;
    }
    // An English sentence: three chunks or more, half of them English or
    // more, and every other one an English word too (going to be, staying
    // home) or one with a slip English Auto puts right (befroe): all of it
    // English. One chunk that is only Japanese (kyouha, suru) keeps the
    // chunks as judged.
    // A chunk that reads both ways, followed by a chunk that starts with a
    // particle (note | ninikki, token | nokigen): Japanese would have been
    // typed on with its particle (notenini), so it is the English word.
    if (particleAfter_) {
        // Or with する after it (share | shitekara), as English nouns take it.
        constexpr std::wstring_view kCaseParticles[] = {L"ni", L"wo", L"ga", L"no", L"ha", L"wa", L"de", L"to",
                                                        L"mo", L"he", L"kara", L"made", L"yori",
                                                        L"suru", L"shi", L"sare", L"sase"};
        for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
            auto& j = judged[i];
            if (j.english || !j.either || judged[i + 1].english) continue;
            const std::wstring_view next = chunks[i + 1];
            const bool particle = std::any_of(std::begin(kCaseParticles), std::end(kCaseParticles),
                                              [&](std::wstring_view p) { return next.starts_with(p); });
            if (!particle) continue;
            j.english = true;
            j.byEnglishAround = true;
            j.written = WrittenAs(chunks[i]);
        }
    }
    constexpr std::size_t kSentenceChunks = 3;
    // English only for the English around it does not make the sentence
    // English (game no server gaomoi).
    const auto english = static_cast<std::size_t>(std::count_if(
        judged.begin(), judged.end(), [](const Judgement& j) { return j.english && !j.byEnglishAround; }));
    // Or every chunk an English word, though each reads as romaji too
    // (take the same road, i made it home).
    const bool allWords = std::all_of(judged.begin(), judged.end(), [](const Judgement& j) { return j.english || j.either; });
    // Not with a chunk in it that reads as Japanese only (github no issue
    // nikaita: nikaita is no English).
    const bool japaneseOnly = englishAround_ && !allWords;
    if (chunks.size() >= kSentenceChunks && !japaneseOnly && (english * 2 >= chunks.size() || (englishAround_ && allWords))) {
        std::wstring text;
        for (const auto& j : judged) {
            if (j.english) text += j.written + L" ";
        }
        auto all = judged;
        const auto isLetters = [](std::wstring_view keys) {
            return !keys.empty() && std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
        };
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            if (all[i].english) continue;
            const std::wstring_view keys = chunks[i];
            if (!isLetters(keys)) {
                // Numbers and words with a hyphen or colon (6, 4th,
                // double-check, haul:) as typed.
                const bool typed = !keys.empty() && std::any_of(keys.begin(), keys.end(), [](wchar_t c) {
                    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
                }) && std::all_of(keys.begin(), keys.end(), [](wchar_t c) {
                    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || std::wstring_view(L"'-:;/").find(c) != std::wstring_view::npos;
                });
                if (!typed) return judged;
                // A hyphen typed for ー (noho-mupe-jiwo: のホームページを) is
                // Japanese: the keys read as kana throughout, and a part
                // between hyphens is no English word (double-check is).
                if (keys.find(L'-') != std::wstring_view::npos) {
                    bool kanaThroughout = true;
                    for (const auto& token : JapaneseComposer::ParseRomaji(table_, keys)) {
                        kanaThroughout = kanaThroughout && !token.leftover;
                    }
                    bool allWords = true;
                    for (std::size_t at = 0; at <= keys.size();) {
                        std::size_t end = keys.find(L'-', at);
                        if (end == std::wstring_view::npos) end = keys.size();
                        const auto part = keys.substr(at, end - at);
                        if (!part.empty() && english_.Score(part) < thresholds_.knownWord) allWords = false;
                        at = end + 1;
                    }
                    if (kanaThroughout && !allWords) return judged;
                }
                all[i].english = true;
                all[i].written = std::wstring(keys);
                continue;
            }
            auto again = Judge(keys, kSentenceChunks, false, spelling, text, englishSentences);
            std::wstring kana;
            bool readable = true;
            for (const auto& token : JapaneseComposer::ParseRomaji(table_, keys)) {
                readable = readable && !token.leftover;
                kana += token.kana;
            }
            // Mostly English (five chunks or more, four in five English):
            // a slip that reads as a Japanese word is English too (si: is,
            // yo: you).
            constexpr std::size_t kLongSentence = 5;
            const bool mostlyEnglish = chunks.size() >= kLongSentence && english * 5 >= chunks.size() * 4;
            const bool japaneseWord = !mostlyEnglish && readable && converter_.IsWord(kana);
            // An English word: a common one even when it is Japanese too
            // (to, home, he); a rarer one only when it is not (nan da: 何だ).
            const double score = english_.Score(keys);
            if (!again.english && (score >= thresholds_.commonWord || (!japaneseWord && score >= thresholds_.knownWord))) {
                again.english = true;
                again.word = true;
                again.written = WrittenAs(keys);
            }
            if (!again.english && spelling) {
                // A slip that reads as romaji (apprecaite), but not as a
                // Japanese word (mita: 見た, not Meta).
                if (japaneseWord) return judged;
                if (const auto spelled = spelling->Spell(keys, text); spelled.corrected) {
                    again.english = true;
                    again.word = true;
                    again.written = WrittenAs(spelled.word);
                }
            }
            if (!again.english) return judged;
            all[i] = std::move(again);
        }
        // Written as English Auto writes it: slips put right with the
        // English before (meetin: meeting) and looked at again with the word
        // after (enough tim to: time); names only with capitals inside
        // (GitHub), not words a name list also has (cup, apple).
        if (spelling) {
            // All capitals (API) only for no English word (it: not IT).
            const auto asEnglish = [&](const std::wstring& word) {
                const auto named = WrittenAs(word, false);
                const bool inside = named.size() > 1 && std::any_of(named.begin() + 1, named.end(),
                                                                    [](wchar_t c) { return std::iswupper(c) != 0; });
                const bool allCapitals = std::none_of(named.begin(), named.end(), [](wchar_t c) { return std::iswlower(c) != 0; });
                std::wstring lower = word;
                for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
                const bool acronym = allCapitals && english_.Score(lower) < thresholds_.knownWord;
                return inside && (!allCapitals || acronym) ? named : word;
            };
            std::wstring before;
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                if (isLetters(chunks[i])) {
                    const auto spelled = spelling->Spell(chunks[i], before);
                    std::wstring word = spelled.word;
                    if (!spelled.corrected && i + 1 < chunks.size()) {
                        if (const auto meant = spelling->Recheck(chunks[i], before, chunks[i + 1])) word = *meant;
                    }
                    all[i].written = asEnglish(word);
                }
                before += all[i].written + L" ";
            }
        }
        return all;
    }
    return judged;
}

bool ChunkJudge::EnglishThenJapanese(std::wstring_view keys) const {
    if (english_.Score(keys) >= thresholds_.knownWord) return false;
    if (names_ && !names_->ForEnglish(keys, 1).empty()) return false;
    constexpr std::wstring_view kStarts[] = {L"wo", L"ga", L"ni", L"de", L"ha", L"wa", L"mo", L"no", L"to"};
    for (std::size_t at = 1; at + 2 <= keys.size(); ++at) {
        const auto rest = keys.substr(at);
        if (std::none_of(std::begin(kStarts), std::end(kStarts),
                         [&](std::wstring_view particle) { return rest.starts_with(particle); })) {
            continue;
        }
        bool readable = true;
        for (const auto& token : JapaneseComposer::ParseRomaji(table_, rest)) readable = readable && !token.leftover;
        bool englishReadable = true;
        for (const auto& token : JapaneseComposer::ParseRomaji(table_, keys.substr(0, at))) {
            englishReadable = englishReadable && !token.leftover;
        }
        if (readable && !englishReadable) return true;
    }
    return false;
}

}  // namespace tekito::japanese
