#include "Core/Japanese/JapaneseConverter.h"

#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/LanguageModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace tekito::japanese {
namespace {

bool IsKanaChar(wchar_t c) {
    return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30F6) || c == 0x30FC;
}

bool IsIdeograph(wchar_t c) { return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || c == 0x3005; }

// A character no word covers, and a run of characters the dictionary never
// uses (letters, full-width digits): costly, so words win when they can.
constexpr std::int64_t kUnknownCharacterCost = 10000;
constexpr std::int64_t kUnknownRunCost = 8000;
// Kana shaped like a borrowed word read as one katakana word, up to this
// many characters: dearer the longer, so words still win a long stretch.
constexpr std::int64_t kLoanwordCost = 4000;
constexpr std::int64_t kLoanwordCharacterCost = 1200;
constexpr std::size_t kLongestLoanword = 6;
// Spelling-correction entries are offered, not chosen.
constexpr std::int64_t kSpellingCorrectionPenalty = 5000;
// Transliterations come after every dictionary candidate.
constexpr std::int64_t kTransliterationCost = std::numeric_limits<std::int32_t>::max();
constexpr std::size_t kBestPaths = 12;
constexpr std::int64_t kInfinity = std::numeric_limits<std::int64_t>::max() / 4;
// How far the language model moves a pair, in nats.
constexpr double kPmiFloor = -5.0;
constexpr double kPmiCeiling = 8.0;
// The sentence model reorders only this many of a phrase's candidates.
constexpr std::size_t kTopicCandidates = 8;

struct Node {
    std::uint32_t begin{0};
    std::uint32_t end{0};
    std::uint16_t left{0};
    std::uint16_t right{0};
    std::int64_t cost{0};
    bool known{false};
    DictionaryWord word;
    // A word the user added, and how it is written; or a number read in
    // kana (numeral) with its digits.
    bool user{false};
    bool numeral{false};
    std::wstring text;
    // A word of the dictionary that is a number read in kana and its counter
    // (三時 for さんじ): the number, and how many characters write it.
    std::uint64_t numberValue{0};
    std::size_t numberPrefix{0};
    // For the language model, when it has the word: ln(count), the pair
    // hash the word starts, and its text to end a pair.
    std::optional<double> logCount;
    std::uint64_t pairStart{0};
    std::wstring surface;
};

struct Lattice {
    std::vector<Node> nodes;
    std::vector<std::vector<int>> beginningAt;
    std::vector<std::vector<int>> endingAt;
};

bool IsDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'\xFF10' && c <= L'\xFF19');
}

// What a number read in kana costs over digits typed: words the
// dictionary has for it stay first (二重, 十八番), counters take it (20円).
constexpr std::int64_t kKanaNumberCost = 1000;
// A number of one part before a counter: less than the dictionary's
// numbers left in kana (に, さん: 2歳, not に歳), more than its words (一番).
constexpr std::int64_t kCountedNumberCost = -2000;
// And when a word without a number reads the same (ご縁, 誤解).
constexpr std::int64_t kHomophoneCost = 1500;

// A decimal point or thousands comma, as typed in Japanese.
bool IsNumberMark(wchar_t c) {
    return c == L'.' || c == L',' || c == L'\xFF0E' || c == L'\xFF0C';
}

// Numbers read in kana (にせんご: 2005, さんびゃく: 300, にじゅうよ: 24):
// each length of `reading` from its start that reads as one number of two
// parts or more, and the number. One part alone (いち, じゅう, ひゃく) is
// left to the dictionary (一番, 十分, 百均).
std::vector<std::pair<std::size_t, std::uint64_t>> KanaNumbers(std::wstring_view reading) {
    struct Token {
        std::wstring_view kana;
        std::uint64_t value;
        int rank;  // 0 a digit, 1 十, 2 百, 3 千, 4 万, 8 億
    };
    // Digits as read before a unit too (いっ, ろっ, はっ: いっせん, ろっぴゃく).
    static constexpr Token kTokens[] = {
        {L"いち", 1, 0}, {L"いっ", 1, 0}, {L"に", 2, 0}, {L"さん", 3, 0}, {L"よん", 4, 0}, {L"よ", 4, 0},
        {L"ご", 5, 0}, {L"ろく", 6, 0}, {L"ろっ", 6, 0}, {L"なな", 7, 0}, {L"しち", 7, 0}, {L"はち", 8, 0},
        {L"はっ", 8, 0}, {L"きゅう", 9, 0}, {L"じゅう", 10, 1}, {L"じゅっ", 10, 1}, {L"ひゃく", 100, 2}, {L"びゃく", 100, 2},
        {L"ぴゃく", 100, 2}, {L"せん", 1000, 3}, {L"ぜん", 1000, 3}, {L"まん", 10000, 4}, {L"おく", 100000000, 8},
    };
    std::vector<std::pair<std::size_t, std::uint64_t>> found;
    // `group`: the part below 万 so far; `rank`: the smallest unit in it;
    // `digit`: a digit read and not yet multiplied (0 none); `sokuon`: it
    // was read with っ and needs a unit after it; `big`: the last 万 or 億.
    const auto step = [&](auto&& self, std::size_t at, std::uint64_t total, std::uint64_t group, int rank,
                          std::uint64_t digit, bool sokuon, int big, int parts) -> void {
        // Read with っ (さんじゅっ, にじゅういっ): only right before a counter
        // read so (ぷん, こ, かい: さんじゅっぷん is 30分).
        static constexpr std::wstring_view kAfterSokuon[] = {
            L"ぷん", L"こ", L"かい", L"さい", L"ぽん", L"ぱい", L"てん", L"しゅうかん", L"かげつ",
            L"ぱーせんと", L"きろ", L"せんち", L"ぺーじ", L"ぴき", L"ぱつ"};
        const bool counted = !sokuon || std::any_of(std::begin(kAfterSokuon), std::end(kAfterSokuon),
                                                    [&](std::wstring_view counter) { return reading.substr(at).starts_with(counter); });
        if (parts >= 2 && counted && (group > 0 || digit > 0 || total > 0)) {
            found.emplace_back(at, total + group + digit);
        }
        if (sokuon && digit == 0) return;  // nothing follows じゅっ but its counter
        constexpr int kMostParts = 12;
        if (parts >= kMostParts) return;
        for (const auto& token : kTokens) {
            if (!reading.substr(at).starts_with(token.kana)) continue;
            const std::size_t next = at + token.kana.size();
            if (token.rank == 0) {
                // A digit: not after another digit; only units after a
                // ones digit (にじゅうご, not にじゅうごに).
                if (digit > 0 || rank == 0) continue;
                const bool withSokuon = token.kana.back() == L'っ';
                // よ only last, before a counter (よじかん).
                self(self, next, total, group, rank, token.value, withSokuon, big, parts + 1);
                continue;
            }
            if (token.rank < 4) {
                if (token.rank >= rank) continue;  // units only get smaller
                const std::uint64_t times = digit > 0 ? digit : 1;
                if (digit == 1 && !sokuon) continue;  // いちじゅう, いちひゃく are not read so
                // The unit as read after the digit: ひゃく, さんびゃく,
                // ろっぴゃく; せん, さんぜん, いっせん (not ごぜん: 午前).
                const wchar_t first = token.kana.front();
                bool read = true;
                if (first == L'ひ') read = !sokuon && digit != 3 && digit != 6 && digit != 8;
                if (first == L'び') read = !sokuon && digit == 3;
                if (first == L'ぴ') read = sokuon;
                if (first == L'せ') read = sokuon ? digit != 6 : digit != 3;
                if (first == L'ぜ') read = !sokuon && digit == 3;
                if (first == L'じ') read = !sokuon;
                if (!read) continue;
                self(self, next, total, group + times * token.value, token.rank, 0, token.kana.back() == L'っ', big, parts + 1);
                continue;
            }
            // 万, 億: a part before it, and each only once, larger first.
            if (sokuon || (group == 0 && digit == 0) || token.rank >= big) continue;
            self(self, next, total + (group + digit) * token.value, 0, 4, 0, false, token.rank, parts + 1);
        }
    };
    step(step, 0, 0, 0, 4, 0, false, 9, 0);
    return found;
}

// A number of one part read in kana right before a counter (じゅうじかん:
// 10時間, くがつ: 9月, じゅっぷん: 10分): its length and the number. The
// digit as read before each counter (いち only where no word has it: 一日,
// 一度, 一時 stay; く and し only before がつ and じ,
// よ before じ and にん; いっ, ろっ, はっ, じゅっ before a p, k, s or t).
// How a number of one part (1-9, 10, 100, 1000) is written in kanji.
std::wstring SmallKanji(std::uint64_t value) {
    constexpr std::wstring_view kDigits = L"〇一二三四五六七八九";
    if (value < 10) return std::wstring(1, kDigits[value]);
    if (value == 10) return L"十";
    if (value == 100) return L"百";
    if (value == 1000) return L"千";
    return {};
}

struct Counted {
    std::size_t length;          // of the number
    std::uint64_t value;
    std::size_t counterLength;   // of the counter after it
};
std::vector<Counted> CountedNumber(std::wstring_view reading) {
    struct Read {
        std::wstring_view kana;
        std::uint64_t value;
        std::initializer_list<std::wstring_view> only;  // counters it is read so before; empty: any
    };
    static const Read kRead[] = {
        {L"いち", 1, {L"がつ", L"じかん", L"えん", L"まい", L"ぱーせんと", L"きろ", L"めーとる", L"せんち", L"ばい", L"ぺーじ"}},
        {L"に", 2, {}}, {L"さん", 3, {}}, {L"よん", 4, {}}, {L"よ", 4, {L"じ", L"にん", L"ねん"}},
        {L"し", 4, {L"がつ"}}, {L"ご", 5, {}}, {L"ろく", 6, {}}, {L"なな", 7, {}}, {L"しち", 7, {L"がつ", L"じ"}},
        {L"はち", 8, {}}, {L"きゅう", 9, {}}, {L"く", 9, {L"がつ", L"じ"}}, {L"じゅう", 10, {}},
        {L"ひゃく", 100, {}}, {L"せん", 1000, {}},
        {L"いっ", 1, {L"ぷん", L"こ", L"かい", L"さい", L"ぽん", L"ぱい", L"てん", L"しゅうかん", L"かげつ", L"ぱーせんと", L"きろ", L"せんち", L"ぺーじ"}},
        {L"ろっ", 6, {L"ぷん", L"こ", L"かい", L"ぽん", L"ぱい", L"ぱーせんと", L"ぺーじ"}},
        {L"はっ", 8, {L"ぷん", L"こ", L"かい", L"さい", L"ぽん", L"ぱい", L"てん", L"しゅうかん", L"かげつ", L"ぱーせんと", L"せんち", L"ぺーじ"}},
        {L"じゅっ", 10, {L"ぷん", L"こ", L"かい", L"さい", L"ぽん", L"ぱい", L"てん", L"しゅうかん", L"かげつ", L"ぱーせんと", L"きろ", L"せんち", L"ぺーじ"}},
    };
    // Counters a number in digits is written before.
    static constexpr std::wstring_view kCounters[] = {
        L"じかん", L"じ", L"ふん", L"ぷん", L"びょう", L"がつ", L"かげつ", L"ねん", L"にち", L"しゅうかん",
        L"にん", L"めい", L"こ", L"かい", L"えん", L"てん", L"さい", L"きろ", L"めーとる", L"せんち", L"ぱーせんと",
        L"まい", L"ほん", L"ぼん", L"ぽん", L"さつ", L"だい", L"ばい", L"ぺーじ", L"ぱい", L"ど",
    };
    std::vector<Counted> found;
    for (const auto& read : kRead) {
        if (!reading.starts_with(read.kana)) continue;
        const auto rest = reading.substr(read.kana.size());
        std::size_t counter = 0;
        for (const auto candidate : kCounters) {
            const bool allowed = read.only.size() == 0 ||
                                 std::any_of(read.only.begin(), read.only.end(), [&](std::wstring_view only) { return only == candidate; });
            if (allowed && rest.starts_with(candidate)) counter = std::max(counter, candidate.size());
        }
        if (counter > 0) found.push_back({read.kana.size(), read.value, counter});
    }
    return found;
}

Lattice BuildLattice(const JapaneseDictionary& dictionary, const ReadingCodes& codes,
                     const std::vector<bool>& fixedBoundary, std::wstring_view reading,
                     const LanguageModel* model, const JapaneseUserDictionary* user,
                     const JapaneseConverter::NumberWord* number) {
    const std::size_t n = codes.size();
    Lattice lattice;
    lattice.beginningAt.resize(n + 1);
    lattice.endingAt.resize(n + 1);
    const auto crosses = [&](std::size_t begin, std::size_t end) {
        for (std::size_t p = begin + 1; p < end; ++p) {
            if (fixedBoundary[p]) return true;
        }
        return false;
    };
    const auto add = [&](Node node) {
        const int index = static_cast<int>(lattice.nodes.size());
        lattice.beginningAt[node.begin].push_back(index);
        lattice.endingAt[node.end].push_back(index);
        lattice.nodes.push_back(std::move(node));
    };
    const std::uint16_t unknown = dictionary.UnknownId();
    // Numbers read in kana, by where they start; none starts inside a
    // longer one (にせんご is not に and せんご).
    std::vector<std::vector<std::pair<std::size_t, std::uint64_t>>> numerals(n);
    // And numbers of one part before a counter, costing more (一番, 一度
    // stay words).
    std::vector<std::vector<Counted>> counted(n);
    // Where the dictionary has such a number and counter in kanji.
    std::vector<std::vector<std::size_t>> numeralWords(n);
    if (number) {
        std::size_t covered = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (i < covered) continue;
            numerals[i] = KanaNumbers(reading.substr(i));
            counted[i] = CountedNumber(reading.substr(i));
            for (const auto& [length, value] : numerals[i]) covered = std::max(covered, i + length);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        dictionary.CommonPrefixSearch(ReadingCodesView(codes).substr(i),
                                      [&](std::size_t length, std::uint32_t record) {
            if (crosses(i, i + length)) return;
            dictionary.ForEachWord(record, [&](const DictionaryWord& word) {
                if (user && user->HasSuppressed()) {
                    const auto part = reading.substr(i, length);
                    if (user->Suppresses(part, dictionary.Surface(word, part))) return;
                }
                Node node;
                node.begin = static_cast<std::uint32_t>(i);
                node.end = static_cast<std::uint32_t>(i + length);
                node.left = word.left;
                node.right = word.right;
                node.cost = word.cost + (word.spellingCorrection ? kSpellingCorrectionPenalty : 0);
                node.known = true;
                node.word = word;
                // A number and its counter the dictionary writes in kanji
                // (三時): its number, for the number style.
                // So too its number alone before the counter (三 then 時; に,
                // a number the dictionary leaves in kana, then 歳).
                for (const auto& [numberLength, value, counterLength] : counted[i]) {
                    const auto part = reading.substr(i, length);
                    const auto surface = dictionary.Surface(word, part);
                    const auto kanji = SmallKanji(value);
                    if (kanji.empty()) continue;
                    const auto digits = std::to_wstring(value);
                    if (numberLength + counterLength == length && (surface.starts_with(kanji) || surface.starts_with(digits))) {
                        node.numberValue = value;
                        node.numberPrefix = surface.starts_with(kanji) ? kanji.size() : digits.size();
                        numeralWords[i].push_back(length);
                    } else if (numberLength == length &&
                               (surface == kanji || surface == std::to_wstring(value) ||
                                (number && word.left == number->left && surface == part))) {
                        node.numberValue = value;
                        node.numberPrefix = surface.size();
                    }
                }
                if (model) {
                    auto surface = dictionary.Surface(word, reading.substr(i, length));
                    node.logCount = model->LogCount(surface);
                    if (node.logCount) {
                        node.pairStart = LanguageModel::PairStart(surface);
                        node.surface = std::move(surface);
                    }
                }
                add(std::move(node));
            });
        });
        if (user) {
            user->ForEachPrefixOf(reading.substr(i), [&](std::size_t length, const JapaneseUserDictionary::Entry& entry) {
                if (crosses(i, i + length)) return;
                Node node;
                node.begin = static_cast<std::uint32_t>(i);
                node.end = static_cast<std::uint32_t>(i + length);
                node.left = entry.left;
                node.right = entry.right;
                node.cost = entry.cost;
                node.user = true;
                node.text = entry.surface;
                if (model) {
                    node.logCount = model->LogCount(entry.surface);
                    if (node.logCount) {
                        node.pairStart = LanguageModel::PairStart(entry.surface);
                        node.surface = entry.surface;
                    }
                }
                add(std::move(node));
            });
        }
        // A number: the digits from here, with the marks between them.
        if (number && IsDigit(reading[i]) && (i == 0 || !IsDigit(reading[i - 1]))) {
            std::size_t end = i;
            while (end < n && (end == i || !fixedBoundary[end]) &&
                   (IsDigit(reading[end]) || (IsNumberMark(reading[end]) && end + 1 < n && IsDigit(reading[end + 1])))) {
                ++end;
            }
            Node digits;
            digits.begin = static_cast<std::uint32_t>(i);
            digits.end = static_cast<std::uint32_t>(end);
            digits.left = number->left;
            digits.right = number->right;
            digits.cost = number->cost;
            add(std::move(digits));
        }
        // A number read in kana, written in digits as a number word
        // (にせんごねん: 2005年); the number style is the composer's.
        if (number) {
            for (const auto& [length, value, counterLength] : counted[i]) {
                if (crosses(i, i + length)) continue;
                // The dictionary's word (三時) is the number then.
                if (std::find(numeralWords[i].begin(), numeralWords[i].end(), length + counterLength) != numeralWords[i].end()) {
                    continue;
                }
                // A word read the same with no number in it (ごえん: ご縁,
                // ごかい: 誤解, にじ: 虹): the number costs more, the words
                // around decide.
                bool homophone = false;
                if (const auto record = dictionary.Find(ReadingCodesView(codes).substr(i, length + counterLength))) {
                    const auto part = reading.substr(i, length + counterLength);
                    dictionary.ForEachWord(*record, [&](const DictionaryWord& word) {
                        const auto surface = dictionary.Surface(word, part);
                        homophone = homophone || std::none_of(surface.begin(), surface.end(), [](wchar_t c) {
                            return IsDigit(c) || std::wstring_view(L"〇一二三四五六七八九十百千万").find(c) != std::wstring_view::npos;
                        });
                    });
                }
                Node numeral;
                numeral.begin = static_cast<std::uint32_t>(i);
                numeral.end = static_cast<std::uint32_t>(i + length);
                numeral.left = number->left;
                numeral.right = number->right;
                numeral.cost = number->cost + kCountedNumberCost + (homophone ? kHomophoneCost : 0);
                numeral.user = true;
                numeral.numeral = true;
                numeral.text = std::to_wstring(value);
                add(std::move(numeral));
            }
            for (const auto& [length, value] : numerals[i]) {
                if (crosses(i, i + length)) continue;
                Node numeral;
                numeral.begin = static_cast<std::uint32_t>(i);
                numeral.end = static_cast<std::uint32_t>(i + length);
                numeral.left = number->left;
                numeral.right = number->right;
                numeral.cost = number->cost + kKanaNumberCost;
                numeral.user = true;
                numeral.numeral = true;
                numeral.text = std::to_wstring(value);
                add(std::move(numeral));
            }
        }
        Node single;
        single.begin = static_cast<std::uint32_t>(i);
        single.end = static_cast<std::uint32_t>(i + 1);
        single.left = single.right = unknown;
        single.cost = kUnknownCharacterCost;
        add(std::move(single));
        if (codes[i] == 0 && (i == 0 || codes[i - 1] != 0)) {
            std::size_t end = i;
            while (end < n && codes[end] == 0 && (end == i || !fixedBoundary[end])) ++end;
            if (end - i > 1) {
                Node run;
                run.begin = static_cast<std::uint32_t>(i);
                run.end = static_cast<std::uint32_t>(end);
                run.left = run.right = unknown;
                run.cost = kUnknownRunCost;
                add(std::move(run));
            }
        }
    }
    // Kana shaped like a word from another language, ending in っく, っと and
    // the like (ぐろっく, ぴあっつ): one katakana word too, dearer than the
    // dictionary's words, so a name it lacks (グロック) converts as typed, not
    // as other words or a slip undone (ブロック).
    const auto kana = [](wchar_t c) { return c >= L'\x3041' && c <= L'\x3096'; };
    constexpr std::wstring_view kNoStart = L"っんーぁぃぅぇぉゃゅょゎ";
    constexpr std::wstring_view kAfterSmallTsu = L"くとぷちつすどぐじずぶ";
    constexpr std::wstring_view kParticles = L"のはがをにでともへや";
    for (std::size_t i = 0; i < n; ++i) {
        if (!kana(reading[i]) || kNoStart.find(reading[i]) != std::wstring_view::npos) continue;
        for (std::size_t end = i + 1; end <= n && end - i <= kLongestLoanword; ++end) {
            if (!kana(reading[end - 1]) || crosses(i, end)) break;
            // Not over a particle (脇のハッチ: not ワキノハッチ); one may start it
            // (トップ) or end it after っ (グロット).
            if (end - 1 > i && kParticles.find(reading[end - 1]) != std::wstring_view::npos &&
                reading[end - 2] != L'っ') {
                break;
            }
            const std::size_t length = end - i;
            const bool smallTsuEnd = length >= 3 && reading[end - 2] == L'っ' &&
                                     kAfterSmallTsu.find(reading[end - 1]) != std::wstring_view::npos;
            if (!smallTsuEnd) continue;
            Node loanword;
            loanword.begin = static_cast<std::uint32_t>(i);
            loanword.end = static_cast<std::uint32_t>(end);
            loanword.left = loanword.right = unknown;
            loanword.cost = kLoanwordCost + kLoanwordCharacterCost * static_cast<std::int64_t>(length);
            loanword.user = true;
            loanword.text = ToKatakana(reading.substr(i, length));
            add(std::move(loanword));
        }
    }
    return lattice;
}

// What the language model takes off for `right` following `left`.
// A pair never seen is as if seen half a time: negative for words common
// enough that they would have met; no evidence either way otherwise.
std::int64_t PairBonus(const LanguageModel* model, const JapaneseConverter::ModelWeights& weights, const Node& left,
                       const Node& right) {
    if (!model || !left.logCount || !right.logCount) return 0;
    double pmi = 0.0;
    if (const auto seen = model->PairPmi(left.pairStart, right.surface)) {
        pmi = *seen >= weights.threshold ? *seen - weights.threshold : std::min(0.0, *seen);
    } else {
        pmi = std::min(0.0, std::log(0.5) + model->LogTokens() - *left.logCount - *right.logCount);
    }
    pmi = std::clamp(pmi, kPmiFloor, kPmiCeiling);
    return std::llround(pmi * static_cast<double>(pmi >= 0 ? weights.costPerNat : weights.penaltyPerNat));
}

// The likeliest path through `lattice` for a reading of `n` characters
// after `context`, and its cost.
struct Searched {
    std::vector<int> path;
    std::int64_t cost{kInfinity};
};
Searched SearchPath(const Lattice& lattice, std::size_t n, std::uint16_t context, const ConnectionMatrix& matrix,
                    const LanguageModel* model, const JapaneseConverter::ModelWeights& weights) {
    const auto& nodes = lattice.nodes;
    std::vector<std::int64_t> best(nodes.size(), kInfinity);
    std::vector<int> previous(nodes.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const int k : lattice.beginningAt[i]) {
            const Node& node = nodes[k];
            if (i == 0) {
                best[k] = node.cost + matrix.Cost(context, node.left);
                continue;
            }
            for (const int p : lattice.endingAt[i]) {
                if (best[p] >= kInfinity) continue;
                const std::int64_t cost = best[p] + matrix.Cost(nodes[p].right, node.left) + node.cost -
                                          PairBonus(model, weights, nodes[p], node);
                if (cost < best[k]) {
                    best[k] = cost;
                    previous[k] = p;
                }
            }
        }
    }
    Searched searched;
    int last = -1;
    for (const int p : lattice.endingAt[n]) {
        if (best[p] >= kInfinity) continue;
        const std::int64_t cost = best[p] + matrix.Cost(nodes[p].right, 0);
        if (cost < searched.cost) {
            searched.cost = cost;
            last = p;
        }
    }
    for (int k = last; k >= 0; k = previous[k]) searched.path.push_back(k);
    std::reverse(searched.path.begin(), searched.path.end());
    return searched;
}

struct PathEntry {
    std::int64_t cost{kInfinity};
    int previous{-1};
    int previousRank{-1};
};

}  // namespace

std::vector<Phrase> JapaneseConverter::Convert(std::wstring_view reading,
                                               std::span<const std::size_t> fixedLengths,
                                               std::uint16_t context,
                                               std::span<const std::wstring> contextWords,
                                               const JapaneseUserDictionary* user,
                                               std::span<const std::size_t> breaks) const {
    const std::size_t n = reading.size();
    if (n == 0) return {};
    const ReadingCodes codes = dictionary_.Encode(reading);

    std::vector<bool> fixedBoundary(n + 1, false);
    std::size_t fixedEnd = 0;
    for (const std::size_t length : fixedLengths) {
        if (length == 0 || fixedEnd + length > n) break;
        fixedEnd += length;
        fixedBoundary[fixedEnd] = true;
    }
    for (const std::size_t at : breaks) {
        if (at > 0 && at < n) fixedBoundary[at] = true;
    }
    const bool pairs = model_ && (weights_.costPerNat > 0 || weights_.penaltyPerNat > 0);
    const Lattice lattice = BuildLattice(dictionary_, codes, fixedBoundary, reading, pairs ? model_ : nullptr, user,
                                         number_ ? &*number_ : nullptr);
    const auto& nodes = lattice.nodes;

    // The most likely words for the whole reading.
    const std::vector<int> path = SearchPath(lattice, n, context, matrix_, model_, weights_).path;
    if (path.empty()) return {};
    // Phrases: fixed ones as given, then wherever the rules put a boundary.
    std::vector<std::pair<std::size_t, std::size_t>> spans;  // path index ranges
    std::size_t start = 0;
    for (std::size_t k = 1; k < path.size(); ++k) {
        const Node& before = nodes[path[k - 1]];
        const Node& node = nodes[path[k]];
        const bool split = fixedBoundary[node.begin] ||
                           (node.begin >= fixedEnd && matrix_.IsBoundary(before.right, node.left));
        if (split) {
            spans.emplace_back(start, k);
            start = k;
        }
    }
    spans.emplace_back(start, path.size());

    const auto textOf = [&](const Node& node) {
        const auto part = reading.substr(node.begin, node.end - node.begin);
        if (node.numberPrefix > 0) {
            // In digits, for the number style (三時: 3時).
            return std::to_wstring(node.numberValue) + dictionary_.Surface(node.word, part).substr(node.numberPrefix);
        }
        return node.user ? node.text : node.known ? dictionary_.Surface(node.word, part) : std::wstring(part);
    };
    // The content words of the likeliest reading, by phrase, for the
    // sentence model: each phrase's candidates meet the others' words.
    const bool topics = model_ && weights_.costPerTopic > 0;
    std::vector<std::pair<std::size_t, std::wstring>> sentenceWords;  // (span index, word)
    if (topics) {
        // Words written just before belong to no phrase here.
        for (const auto& word : contextWords) sentenceWords.emplace_back(spans.size(), word);
        for (std::size_t s = 0; s < spans.size(); ++s) {
            for (std::size_t k = spans[s].first; k < spans[s].second; ++k) {
                if (!nodes[path[k]].known && !nodes[path[k]].user) continue;
                auto word = textOf(nodes[path[k]]);
                if (LanguageModel::IsContentWord(word)) sentenceWords.emplace_back(s, std::move(word));
            }
        }
    }

    std::vector<Phrase> phrases;
    std::vector<std::vector<PathEntry>> entries(nodes.size());
    for (std::size_t spanIndex = 0; spanIndex < spans.size(); ++spanIndex) {
        const auto [first, end] = spans[spanIndex];
        const std::uint32_t begin = nodes[path[first]].begin;
        const std::uint32_t finish = nodes[path[end - 1]].end;
        const std::uint16_t leftContext = first == 0 ? context : nodes[path[first - 1]].right;
        const std::uint16_t rightContext = end == path.size() ? 0 : nodes[path[end]].left;

        // The likeliest ways to write this phrase between its neighbors.
        for (std::uint32_t i = begin; i < finish; ++i) {
            for (const int k : lattice.beginningAt[i]) {
                const Node& node = nodes[k];
                auto& list = entries[k];
                list.clear();
                if (node.end > finish) continue;
                if (i == begin) {
                    list.push_back({node.cost + matrix_.Cost(leftContext, node.left), -1, -1});
                } else {
                    for (const int p : lattice.endingAt[i]) {
                        if (nodes[p].begin < begin) continue;
                        const std::int64_t connection =
                            matrix_.Cost(nodes[p].right, node.left) - PairBonus(model_, weights_, nodes[p], node);
                        for (std::size_t r = 0; r < entries[p].size(); ++r) {
                            list.push_back({entries[p][r].cost + connection + node.cost, p,
                                            static_cast<int>(r)});
                        }
                    }
                }
                if (list.size() > kBestPaths) {
                    std::partial_sort(list.begin(), list.begin() + kBestPaths, list.end(),
                                      [](const PathEntry& a, const PathEntry& b) { return a.cost < b.cost; });
                    list.resize(kBestPaths);
                } else {
                    std::sort(list.begin(), list.end(),
                              [](const PathEntry& a, const PathEntry& b) { return a.cost < b.cost; });
                }
            }
        }
        struct Ending {
            std::int64_t cost;
            int node;
            int rank;
        };
        std::vector<Ending> endings;
        for (const int k : lattice.endingAt[finish]) {
            if (nodes[k].begin < begin) continue;
            const int connection = matrix_.Cost(nodes[k].right, rightContext);
            for (std::size_t r = 0; r < entries[k].size(); ++r) {
                endings.push_back({entries[k][r].cost + connection, k, static_cast<int>(r)});
            }
        }
        std::sort(endings.begin(), endings.end(),
                  [](const Ending& a, const Ending& b) { return a.cost < b.cost; });

        Phrase phrase;
        phrase.begin = begin;
        phrase.length = finish - begin;
        const auto has = [&](const std::wstring& text) {
            return std::any_of(phrase.candidates.begin(), phrase.candidates.end(),
                               [&](const PhraseCandidate& c) { return c.text == text; });
        };
        // The content word each candidate stands on (its longest), for the
        // sentence model.
        std::vector<std::wstring> heads;
        for (const auto& ending : endings) {
            if (phrase.candidates.size() >= kBestPaths) break;
            std::wstring text;
            std::wstring head;
            bool spelling = false;
            bool kanaNumber = false;
            bool setPhrase = false;
            std::vector<int> parts;
            for (int k = ending.node, r = ending.rank; k >= 0;) {
                parts.push_back(k);
                const PathEntry& entry = entries[k][static_cast<std::size_t>(r)];
                k = entry.previous;
                r = entry.previousRank;
            }
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                auto part = textOf(nodes[*it]);
                if (topics && (nodes[*it].known || nodes[*it].user) && part.size() > head.size() &&
                    LanguageModel::IsContentWord(part)) {
                    head = part;
                }
                text += part;
                spelling = spelling || (nodes[*it].known && nodes[*it].word.spellingCorrection);
                kanaNumber = kanaNumber || nodes[*it].numeral || nodes[*it].numberPrefix > 0;
                setPhrase = setPhrase || (nodes[*it].known && nodes[*it].word.setPhrase);
            }
            if (!has(text)) {
                PhraseCandidate candidate{std::move(text), ending.cost, PhraseCandidate::Kind::Dictionary, spelling};
                candidate.rightId = nodes[ending.node].right;
                candidate.kanaNumber = kanaNumber;
                candidate.setPhrase = setPhrase;
                phrase.candidates.push_back(std::move(candidate));
                heads.push_back(std::move(head));
            }
        }

        // Every word read exactly like the phrase (homophones), by likelihood.
        const auto phraseReading = reading.substr(begin, finish - begin);
        if (const auto record = dictionary_.Find(ReadingCodesView(codes).substr(begin, finish - begin))) {
            std::vector<PhraseCandidate> words;
            dictionary_.ForEachWord(*record, [&](const DictionaryWord& word) {
                const std::int64_t cost = matrix_.Cost(leftContext, word.left) + word.cost +
                                          (word.spellingCorrection ? kSpellingCorrectionPenalty : 0) +
                                          matrix_.Cost(word.right, rightContext);
                PhraseCandidate candidate{dictionary_.Surface(word, phraseReading), cost,
                                          PhraseCandidate::Kind::Dictionary, word.spellingCorrection};
                candidate.rightId = word.right;
                words.push_back(std::move(candidate));
            });
            std::stable_sort(words.begin(), words.end(),
                             [](const PhraseCandidate& a, const PhraseCandidate& b) { return a.cost < b.cost; });
            for (auto& word : words) {
                if (phrase.candidates.size() >= kMaxCandidates) break;
                if (!has(word.text)) {
                    heads.push_back(topics && LanguageModel::IsContentWord(word.text) ? word.text : std::wstring{});
                    phrase.candidates.push_back(std::move(word));
                }
            }
        }
        if (topics && !heads.empty() && !heads.front().empty()) {
            // When the first choice stands on a content word, the candidates
            // that do too are put in order again with each one's strongest
            // tie to the other phrases' words taken off its cost; the rest
            // keep their places (kana and particles are not told apart so).
            std::vector<std::size_t> places;
            std::vector<std::pair<std::int64_t, PhraseCandidate>> ranked;
            for (std::size_t c = 0; c < phrase.candidates.size() && c < heads.size() && c < kTopicCandidates; ++c) {
                if (heads[c].empty()) continue;
                double strongest = 0.0;
                for (const auto& [where, word] : sentenceWords) {
                    if (where == spanIndex) continue;
                    if (const auto tie = model_->Topic(heads[c], word)) strongest = std::max(strongest, *tie);
                }
                places.push_back(c);
                ranked.emplace_back(phrase.candidates[c].cost - std::llround(strongest * static_cast<double>(weights_.costPerTopic)),
                                    phrase.candidates[c]);
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t r = 0; r < ranked.size(); ++r) phrase.candidates[places[r]] = std::move(ranked[r].second);
        }

        // The user's words read like the phrase come first.
        if (const auto* words = user ? user->Exact(phraseReading) : nullptr) {
            std::vector<PhraseCandidate> userFirst;
            for (const auto& word : *words) {
                PhraseCandidate candidate{word.surface,
                                          matrix_.Cost(leftContext, word.left) + word.cost +
                                              matrix_.Cost(word.right, rightContext),
                                          PhraseCandidate::Kind::Dictionary, false};
                candidate.rightId = word.right;
                userFirst.push_back(std::move(candidate));
            }
            std::stable_sort(userFirst.begin(), userFirst.end(),
                             [](const PhraseCandidate& a, const PhraseCandidate& b) { return a.cost < b.cost; });
            std::erase_if(phrase.candidates, [&](const PhraseCandidate& c) {
                return std::any_of(userFirst.begin(), userFirst.end(), [&](const PhraseCandidate& f) { return f.text == c.text; });
            });
            phrase.candidates.insert(phrase.candidates.begin(), std::make_move_iterator(userFirst.begin()),
                                     std::make_move_iterator(userFirst.end()));
        }

        // The words only to offer come second (moved there if the
        // dictionary has them further down), after the first choice.
        if (const auto* words = user ? user->Suggested(phraseReading) : nullptr) {
            std::size_t at = std::min<std::size_t>(1, phrase.candidates.size());
            for (const auto& word : *words) {
                const auto found = std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                                [&](const PhraseCandidate& c) { return c.text == word.surface; });
                const auto index = static_cast<std::size_t>(found - phrase.candidates.begin());
                if (found != phrase.candidates.end() && index < at) continue;  // already first
                PhraseCandidate candidate{word.surface,
                                          matrix_.Cost(leftContext, word.left) + word.cost +
                                              matrix_.Cost(word.right, rightContext),
                                          PhraseCandidate::Kind::Dictionary, false};
                candidate.rightId = word.right;
                if (found != phrase.candidates.end()) phrase.candidates.erase(found);
                phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(at++),
                                         std::move(candidate));
            }
        }

        const std::wstring hiragana(phraseReading);
        const std::wstring katakana = ToKatakana(phraseReading);
        if (!has(hiragana)) {
            phrase.candidates.push_back({hiragana, kTransliterationCost, PhraseCandidate::Kind::Hiragana, false});
        }
        if (!has(katakana)) {
            phrase.candidates.push_back({katakana, kTransliterationCost, PhraseCandidate::Kind::Katakana, false});
        }
        // What the user never wants for this reading goes; the kana stay if
        // nothing else would.
        if (user && user->HasSuppressed()) {
            std::erase_if(phrase.candidates,
                          [&](const PhraseCandidate& c) { return user->Suppresses(phraseReading, c.text); });
            if (phrase.candidates.empty()) {
                phrase.candidates.push_back({hiragana, kTransliterationCost, PhraseCandidate::Kind::Hiragana, false});
            }
        }
        phrases.push_back(std::move(phrase));
    }
    return phrases;
}

bool JapaneseConverter::IsWord(std::wstring_view reading) const {
    if (reading.empty()) return false;
    bool found = false;
    dictionary_.CommonPrefixSearch(dictionary_.Encode(reading), [&](std::size_t length, std::uint32_t) {
        found = found || length == reading.size();
    });
    return found;
}

std::optional<std::int64_t> JapaneseConverter::Cost(std::wstring_view reading, std::uint16_t context,
                                                   const JapaneseUserDictionary* user) const {
    const std::size_t n = reading.size();
    if (n == 0) return std::nullopt;
    const ReadingCodes codes = dictionary_.Encode(reading);
    const std::vector<bool> noBoundary(n + 1, false);
    const bool pairs = model_ && (weights_.costPerNat > 0 || weights_.penaltyPerNat > 0);
    const Lattice lattice = BuildLattice(dictionary_, codes, noBoundary, reading, pairs ? model_ : nullptr, user,
                                         number_ ? &*number_ : nullptr);
    const auto searched = SearchPath(lattice, n, context, matrix_, model_, weights_);
    if (searched.path.empty()) return std::nullopt;
    return searched.cost;
}

std::optional<PhraseCandidate> JapaneseConverter::Best(std::wstring_view reading, std::uint16_t context,
                                                      const JapaneseUserDictionary* user) const {
    const std::size_t n = reading.size();
    if (n == 0) return std::nullopt;
    const ReadingCodes codes = dictionary_.Encode(reading);
    const std::vector<bool> noBoundary(n + 1, false);
    // Quick on purpose: the language model is left out.
    const Lattice lattice = BuildLattice(dictionary_, codes, noBoundary, reading, nullptr, user,
                                         number_ ? &*number_ : nullptr);
    const auto& nodes = lattice.nodes;
    std::vector<std::int64_t> best(nodes.size(), kInfinity);
    std::vector<int> previous(nodes.size(), -1);
    for (std::size_t i = 0; i < n; ++i) {
        for (const int k : lattice.beginningAt[i]) {
            const Node& node = nodes[k];
            if (!node.known && !node.user) continue;
            if (i == 0) {
                best[k] = node.cost + matrix_.Cost(context, node.left);
                continue;
            }
            for (const int p : lattice.endingAt[i]) {
                if (best[p] >= kInfinity) continue;
                const std::int64_t cost = best[p] + matrix_.Cost(nodes[p].right, node.left) + node.cost;
                if (cost < best[k]) {
                    best[k] = cost;
                    previous[k] = p;
                }
            }
        }
    }
    int last = -1;
    std::int64_t lastCost = kInfinity;
    for (const int p : lattice.endingAt[n]) {
        if (best[p] >= kInfinity) continue;
        const std::int64_t cost = best[p] + matrix_.Cost(nodes[p].right, 0);
        if (cost < lastCost) {
            lastCost = cost;
            last = p;
        }
    }
    if (last < 0) return std::nullopt;
    std::vector<int> path;
    for (int k = last; k >= 0; k = previous[k]) path.push_back(k);
    PhraseCandidate result{{}, lastCost, PhraseCandidate::Kind::Dictionary, false};
    result.rightId = nodes[last].right;
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const Node& node = nodes[*it];
        result.text += node.user ? node.text
                                 : dictionary_.Surface(node.word, reading.substr(node.begin, node.end - node.begin));
        result.spellingCorrection = result.spellingCorrection || node.word.spellingCorrection;
    }
    // A text the user never wants for this reading is not the likeliest.
    if (user && user->Suppresses(reading, result.text)) return std::nullopt;
    return result;
}

std::vector<Prediction> JapaneseConverter::Predict(std::wstring_view reading, std::size_t limit,
                                                  const JapaneseUserDictionary* user) const {
    // Going through more keys than this would take too long per keystroke.
    constexpr std::uint32_t kMaxKeys = 2000;
    std::vector<Prediction> predictions;
    if (reading.empty() || limit == 0) return predictions;
    if (user) {
        for (const auto* entry : user->StartingWith(reading)) {
            if (predictions.size() >= limit) break;
            predictions.push_back({entry->reading, entry->surface, entry->cost});
        }
    }
    const ReadingCodes codes = dictionary_.Encode(reading);
    auto ranges = dictionary_.PrefixRanges(codes);
    // Too many to look through: the dictionary's words are left out.
    if (ranges[0].second - ranges[0].first + ranges[1].second - ranges[1].first > kMaxKeys) ranges = {};

    struct Found {
        std::int64_t cost;
        std::uint32_t record;
        DictionaryWord word;
    };
    std::vector<Found> found;
    for (const auto& [first, end] : ranges) {
        for (std::uint32_t record = first; record < end; ++record) {
            dictionary_.ForEachStoredWord(record, [&](const DictionaryWord& word) {
                // The cheapest word of each longer reading, never a spelling
                // correction.
                if (word.spellingCorrection) return true;
                found.push_back({word.cost, record, word});
                return false;
            });
        }
    }
    const auto cheaper = [](const Found& a, const Found& b) { return a.cost < b.cost; };
    // Only the first few are needed; some may be dropped below.
    const std::size_t sorted = std::min(found.size(), limit * 4);
    std::partial_sort(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(sorted), found.end(), cheaper);
    found.resize(sorted);
    for (const auto& item : found) {
        std::wstring key = dictionary_.KeyText(item.record);
        if (key.size() <= reading.size()) continue;
        std::wstring text = dictionary_.Surface(item.word, key);
        if (user && user->Suppresses(key, text)) continue;
        const bool seen = std::any_of(predictions.begin(), predictions.end(),
                                      [&](const Prediction& p) { return p.text == text; });
        if (seen) continue;
        // Twice as many, for the other endings to go after (below).
        if (predictions.size() >= limit * 2) break;
        predictions.push_back({std::move(key), std::move(text), item.cost});
    }
    // Other endings of a word already there (借りたかっ, 借りたく after
    // 借りたい) come after the other words.
    {
        const auto stem = [](const std::wstring& text) {
            std::size_t end = text.size();
            while (end > 0 && ((text[end - 1] >= L'\x3041' && text[end - 1] <= L'\x3096') || text[end - 1] == L'ー')) --end;
            return end == 0 ? text : text.substr(0, end);
        };
        // Of each word, the ending a sentence can stop on (面白い, not
        // 面白かっ) stays where the word's first ending was.
        constexpr std::wstring_view kFinal = L"うくすつぬふむゆるぐずぶぷい";
        const auto final = [&](const std::wstring& text) {
            return !text.empty() && kFinal.find(text.back()) != std::wstring_view::npos;
        };
        std::vector<std::wstring> stems(predictions.size());
        for (std::size_t i = 0; i < predictions.size(); ++i) stems[i] = stem(predictions[i].text);
        std::vector<bool> front(predictions.size(), false);
        for (std::size_t i = 0; i < predictions.size(); ++i) {
            if (stems[i] == predictions[i].text) {
                front[i] = true;
                continue;
            }
            const auto seen = [&](std::size_t j) { return stems[j] == stems[i]; };
            bool earlier = false;
            for (std::size_t j = 0; j < i && !earlier; ++j) earlier = seen(j);
            if (earlier) continue;
            // The word's first: it or a later ending of it that can stop.
            std::size_t pick = i;
            for (std::size_t j = i; j < predictions.size(); ++j) {
                if (seen(j) && final(predictions[j].text)) {
                    pick = j;
                    break;
                }
            }
            if (pick != i) {
                std::swap(predictions[i], predictions[pick]);
                std::swap(stems[i], stems[pick]);
            }
            front[i] = true;
        }
        std::vector<Prediction> first, later;
        for (std::size_t i = 0; i < predictions.size(); ++i) (front[i] ? first : later).push_back(std::move(predictions[i]));
        first.insert(first.end(), std::make_move_iterator(later.begin()), std::make_move_iterator(later.end()));
        predictions = std::move(first);
        if (predictions.size() > limit) predictions.resize(limit);
    }
    // The words only to offer come second.
    if (user) {
        std::size_t at = std::min<std::size_t>(1, predictions.size());
        for (const auto* entry : user->StartingWith(reading, true)) {
            const auto existing = std::find_if(predictions.begin(), predictions.end(),
                                               [&](const Prediction& p) { return p.text == entry->surface; });
            if (existing != predictions.end() && static_cast<std::size_t>(existing - predictions.begin()) < at) continue;
            if (existing != predictions.end()) predictions.erase(existing);
            predictions.insert(predictions.begin() + static_cast<std::ptrdiff_t>(at++),
                               {entry->reading, entry->surface, entry->cost});
        }
        if (predictions.size() > limit) predictions.resize(limit);
    }
    return predictions;
}

std::optional<ReadingGuess> JapaneseConverter::ReadingOf(std::wstring_view text) const {
    const std::size_t n = text.size();
    if (n == 0 || n > 256) return std::nullopt;
    struct Word {
        std::size_t length;
        std::wstring reading;
        std::uint16_t left;
        std::uint16_t right;
        std::int64_t cost;
    };
    std::vector<std::vector<Word>> words(n);
    std::vector<bool> present(0x10000, false);
    bool written = false;
    for (const wchar_t c : text) {
        present[static_cast<std::uint16_t>(c)] = true;
        written = written || !IsKanaChar(c);
    }
    // Words written with kanji: one pass over the dictionary.
    if (written) {
        for (std::uint32_t record = 0; record < dictionary_.KeyCount(); ++record) {
            std::wstring reading;
            dictionary_.ForEachStoredWord(record, [&](const DictionaryWord& word) {
                if (word.spellingCorrection) return true;
                const std::size_t length = dictionary_.PoolSurfaceLength(word);
                if (length == 0 || length > n) return true;
                const wchar_t first = dictionary_.PoolSurfaceChar(word, 0);
                if (!present[static_cast<std::uint16_t>(first)]) return true;
                for (std::size_t at = 0; at + length <= n; ++at) {
                    if (text[at] != first) continue;
                    std::size_t i = 1;
                    while (i < length && text[at + i] == dictionary_.PoolSurfaceChar(word, i)) ++i;
                    if (i < length) continue;
                    if (reading.empty()) reading = dictionary_.KeyText(record);
                    if (reading.empty()) return false;
                    words[at].push_back({length, reading, word.left, word.right, word.cost});
                }
                return true;
            });
        }
    }
    // Words written in kana: looked up by their reading.
    const std::wstring hiragana = ToHiragana(text);
    const ReadingCodes codes = dictionary_.Encode(hiragana);
    for (std::size_t at = 0; at < n; ++at) {
        if (!IsKanaChar(text[at])) continue;
        dictionary_.CommonPrefixSearch(ReadingCodesView(codes).substr(at), [&](std::size_t length, std::uint32_t record) {
            if (at + length > n) return;
            const std::wstring_view written(text.data() + at, length);
            const std::wstring reading = hiragana.substr(at, length);
            dictionary_.ForEachWord(record, [&](const DictionaryWord& word) {
                if (word.spellingCorrection || word.surface == DictionaryWord::Surface::Pool) return;
                if (dictionary_.Surface(word, reading) != written) return;
                words[at].push_back({length, reading, word.left, word.right, word.cost});
            });
        });
    }
    // Anything else reads as itself, at a price.
    constexpr std::int64_t kUnknownKana = 6000;
    constexpr std::int64_t kUnknownOther = 12000;
    for (std::size_t at = 0; at < n; ++at) {
        const wchar_t c = text[at];
        if (IsIdeograph(c)) continue;
        words[at].push_back({1, IsKanaChar(c) ? ToHiragana(std::wstring(1, c)) : std::wstring(1, c),
                             dictionary_.UnknownId(), dictionary_.UnknownId(),
                             IsKanaChar(c) ? kUnknownKana : kUnknownOther});
    }
    // The cheapest path, with the words' connection costs.
    struct State {
        std::int64_t cost;
        std::size_t from;     // position the word starts at
        std::uint16_t prior;  // right id before it
        std::size_t word;     // index in words[from]
    };
    std::vector<std::map<std::uint16_t, State>> best(n + 1);
    best[0][0] = {0, 0, 0, 0};
    for (std::size_t at = 0; at < n; ++at) {
        for (const auto& [right, state] : best[at]) {
            for (std::size_t w = 0; w < words[at].size(); ++w) {
                const auto& word = words[at][w];
                const std::int64_t cost = state.cost + matrix_.Cost(right, word.left) + word.cost;
                auto& slot = best[at + word.length];
                const auto found = slot.find(word.right);
                if (found == slot.end() || cost < found->second.cost) slot[word.right] = {cost, at, right, w};
            }
        }
    }
    if (best[n].empty()) return std::nullopt;
    std::uint16_t right = 0;
    std::int64_t total = std::numeric_limits<std::int64_t>::max();
    for (const auto& [id, state] : best[n]) {
        const std::int64_t cost = state.cost + matrix_.Cost(id, 0);
        if (cost < total) {
            total = cost;
            right = id;
        }
    }
    std::vector<const Word*> path;
    for (std::size_t at = n; at > 0;) {
        const State& state = best[at].at(right);
        path.push_back(&words[state.from][state.word]);
        right = state.prior;
        at = state.from;
    }
    std::reverse(path.begin(), path.end());
    ReadingGuess guess;
    for (const auto* word : path) {
        guess.reading += word->reading;
        guess.pieces.emplace_back(word->length, word->reading.size());
    }
    return guess;
}

}  // namespace tekito::japanese
