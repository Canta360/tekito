#include "Core/Japanese/ChunkJudge.h"

#include "Core/EnglishSpelling.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/RankingData.h"
#include "Core/TypoModel.h"

#include <algorithm>
#include <cwctype>
#include <map>

namespace tekito::japanese {

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

std::wstring ChunkJudge::WrittenAs(std::wstring_view word) const {
    std::wstring lower(word);
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    if (names_) {
        if (const auto written = names_->WrittenAs(lower)) return *written;
        for (const auto& spelling : names_->ForEnglish(lower, 1)) {
            const bool capitals = std::any_of(spelling.english.begin() + 1, spelling.english.end(),
                                              [](wchar_t c) { return std::iswupper(c) != 0; });
            if (capitals) return spelling.english;
        }
    }
    return std::wstring(word);
}

ChunkJudge::Judgement ChunkJudge::Judge(std::wstring_view keys, std::size_t englishBefore,
                                        bool japaneseInSentence, const EnglishSpelling* spelling,
                                        std::wstring_view before) const {
    Judgement judgement;
    judgement.written = std::wstring(keys);
    // A word with a number after it (iphone15, gpt4): as the word is, with
    // the number kept.
    std::size_t digits = keys.size();
    while (digits > 0 && keys[digits - 1] >= L'0' && keys[digits - 1] <= L'9') --digits;
    if (digits > 0 && digits < keys.size()) {
        auto word = Judge(keys.substr(0, digits), englishBefore, japaneseInSentence, spelling, before);
        word.written += keys.substr(digits);
        return word;
    }
    const bool letters = !keys.empty() && std::all_of(keys.begin(), keys.end(), [](wchar_t c) {
        return c >= L'a' && c <= L'z';
    });
    if (!letters) return judgement;
    // A first letter typed twice (bbug): no Japanese starts with っ, so the
    // English word, when the rest is one.
    if (keys.size() >= 4 && keys[0] == keys[1] && std::wstring_view(L"aiueon").find(keys[0]) == std::wstring_view::npos) {
        auto rest = Judge(keys.substr(1), englishBefore, japaneseInSentence, spelling, before);
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
        judgement.english = !particle && after && score >= thresholds_.commonWord;
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
        // A slip correction makes Japanese of it: when in doubt, Japanese.
        if (EnglishThenJapanese(keys) || slip) return judgement;
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
    const bool japaneseWord = !katakana && converter_.IsWord(kana);
    const bool japaneseOdd = !japaneseWord &&
                             (!best || kana.empty() || katakana ||
                              best->cost >= thresholds_.costPerKana * static_cast<std::int64_t>(kana.size()));
    // Whether Japanese text writes it in Latin letters (API, Amazon), or
    // hardly ever (take, same). Without the counts, as if it does.
    const auto inJapanese = names_ ? names_->InJapaneseText(keys) : std::nullopt;
    const bool writtenInJapanese = !names_ || !names_->HasJapaneseTextCounts() ||
                                   (inJapanese && *inJapanese >= thresholds_.inJapaneseText);
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
    // English when Japanese text writes it so and it is no Japanese word.
    if (name || score >= thresholds_.knownWord) {
        judgement.either = true;
        judgement.english = englishSentence ||
                            (!japaneseWord && inJapanese && *inJapanese >= thresholds_.inJapaneseText);
    }
    if (!judgement.english) judgement.written = std::wstring(keys);
    return judgement;
}

std::optional<std::vector<ChunkJudge::Piece>> ChunkJudge::Split(std::wstring_view keys) const {
    // An English word costs this many kana of ordinary Japanese; shorter
    // than this many letters only names written with capitals (API).
    constexpr std::int64_t kEnglishKana = 2;
    constexpr std::size_t kShortestWord = 4;
    constexpr std::size_t kMostWords = 24;
    constexpr std::size_t kLongestKeys = 80;
    if (keys.size() < kShortestWord + 2 || keys.size() > kLongestKeys ||
        !std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; })) {
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
    for (std::size_t length = keys.size() - 1; length >= 3; --length) {
        for (std::size_t begin = 0; begin + length <= keys.size() && words.size() < kMostWords; ++begin) {
            const auto word = keys.substr(begin, length);
            bool known = length >= kShortestWord && english_.Score(word) >= thresholds_.knownWord;
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
    std::optional<std::int64_t> best;
    std::vector<Word> chosen;
    // One word, or two apart, with natural Japanese around them.
    const auto tryWords = [&](const std::vector<Word>& pick) {
        std::int64_t cost = wordCost * static_cast<std::int64_t>(pick.size());
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
    constexpr std::int64_t kBetterByKana = 2;
    if (!best || (whole && *best + thresholds_.costPerKana * kBetterByKana >= *whole)) return std::nullopt;
    std::vector<Piece> pieces;
    std::size_t at = 0;
    const auto japanese = [&](std::size_t begin, std::size_t end) {
        if (begin < end) pieces.push_back({false, std::wstring(keys.substr(begin, end - begin)), *kanaOf(keys.substr(begin, end - begin))});
    };
    for (const auto& word : chosen) {
        japanese(at, word.begin);
        const auto typed = keys.substr(word.begin, word.end - word.begin);
        pieces.push_back({true, std::wstring(typed), WrittenAs(typed)});
        at = word.end;
    }
    japanese(at, keys.size());
    return pieces;
}

std::vector<ChunkJudge::Judgement> ChunkJudge::JudgeSentence(const std::vector<std::wstring>& chunks,
                                                             const EnglishSpelling* spelling) const {
    std::vector<Judgement> judged;
    judged.reserve(chunks.size());
    // As typed: left to right.
    std::size_t run = 0;
    bool japanese = false;
    std::wstring englishText;
    for (const auto& keys : chunks) {
        auto judgement = Judge(keys, run, japanese, spelling, englishText);
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
            auto again = Judge(chunks[i], around, otherJapanese, spelling, others);
            if (again.english != judged[i].english) changed = true;
            judged[i] = std::move(again);
        }
        if (!changed) break;
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
