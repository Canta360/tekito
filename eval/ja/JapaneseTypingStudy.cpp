#include "JapaneseTypingStudy.h"

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/RomajiTable.h"
#include "JapaneseEvalCommon.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tekito::japanese::JapaneseComposer;
using tekito::japanese::JapaneseConverter;
using tekito::japanese::JapaneseDictionary;
using tekito::japanese::JapaneseUserDictionary;
using tekito::japanese::Phrase;
using Clock = std::chrono::steady_clock;
using ja_eval::Narrow;
using ja_eval::Percentile;

constexpr std::size_t npos = std::wstring::npos;

double Since(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

// How the text lines up with its reading, and where a typist marking words
// would break it into phrases.
struct Alignment {
    // For each position in the reading (and its end), the position in the
    // text there, or npos inside a run of kanji.
    std::vector<std::size_t> textAt;
    // Reading positions where a phrase starts, in order.
    std::vector<std::size_t> breaks;
};

bool IsHiragana(wchar_t c) {
    return c >= 0x3041 && c <= 0x3096;
}
bool IsKatakana(wchar_t c) {
    return (c >= 0x30A1 && c <= 0x30F6) || c == 0x30FC;
}

std::optional<Alignment> Align(const JapaneseDictionary& dictionary, std::wstring_view reading, std::wstring_view text,
                               bool katakanaAsKanji) {
    enum class Kind { Kana, Content, Mark };
    struct Element {
        std::size_t begin, end;  // in the text
        wchar_t literal;         // 0: any reading of one or more kana (kanji, digits, letters)
        Kind kind;
    };
    std::vector<Element> elements;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        wchar_t literal = 0;
        Kind kind = Kind::Content;
        if (IsHiragana(c)) {
            literal = c;
            kind = Kind::Kana;
        } else if (c == 0x30FC) {
            // A long mark belongs with what it lengthens.
            literal = c;
            kind = elements.empty() ? Kind::Kana : elements.back().kind;
        } else if (IsKatakana(c)) {
            if (!katakanaAsKanji) literal = tekito::japanese::ToHiragana(std::wstring_view(&c, 1))[0];
        } else if (!(c >= 0x3400 && c <= 0x9FFF) && !(c >= 0xF900 && c <= 0xFAFF) && c != 0x3005 && !std::iswalnum(c) &&
                   !(c >= 0xFF10 && c <= 0xFF5A)) {
            literal = tekito::japanese::ToFullWidthAscii(std::wstring_view(&c, 1))[0];
            kind = Kind::Mark;
        }
        if (literal == 0 && !elements.empty() && elements.back().literal == 0) {
            elements.back().end = i + 1;
            continue;
        }
        elements.push_back({i, i + 1, literal, kind});
    }

    // The likeliest way the kanji runs are read: as the dictionary writes
    // that reading, else about two kana a kanji, never from a kana no word
    // starts with.
    const std::size_t n = reading.size();
    const auto codes = dictionary.Encode(reading);
    constexpr int kFailed = 1 << 29;
    std::vector<int> cost((elements.size() + 1) * (n + 1), -1);
    std::vector<std::size_t> taken((elements.size() + 1) * (n + 1), 0);
    std::function<int(std::size_t, std::size_t)> match = [&](std::size_t e, std::size_t r) -> int {
        if (e == elements.size()) return r == n ? 0 : kFailed;
        int& best = cost[e * (n + 1) + r];
        if (best >= 0) return best;
        best = kFailed;
        const auto& element = elements[e];
        if (element.literal != 0) {
            if (r < n && reading[r] == element.literal) best = match(e + 1, r + 1);
            taken[e * (n + 1) + r] = 1;
            return best;
        }
        const std::size_t m = element.end - element.begin;
        if (r < n && std::wstring_view(L"んっゃゅょぁぃぅぇぉゎー").find(reading[r]) != std::wstring_view::npos) {
            return best;
        }
        std::set<std::size_t> written;  // lengths the dictionary writes as this run
        const auto surface = text.substr(element.begin, m);
        const auto from = tekito::japanese::ReadingCodesView(codes).substr(r);
        dictionary.CommonPrefixSearch(from, [&](std::size_t length, std::uint32_t record) {
            dictionary.ForEachWord(record, [&](const auto& word) {
                if (dictionary.Surface(word, reading.substr(r, length)) == surface) written.insert(length);
            });
        });
        for (std::size_t k = m; r + k <= n; ++k) {
            const int rest = match(e + 1, r + k);
            if (rest >= kFailed) continue;
            const int here = written.count(k) ? 0 : 10 + std::abs(static_cast<int>(k) - 2 * static_cast<int>(m));
            if (rest + here < best) {
                best = rest + here;
                taken[e * (n + 1) + r] = k;
            }
        }
        return best;
    };
    if (match(0, 0) >= kFailed) return std::nullopt;

    Alignment alignment;
    alignment.textAt.assign(n + 1, npos);
    std::vector<std::size_t> starts(elements.size() + 1);  // reading position of each element
    for (std::size_t e = 0, r = 0; e <= elements.size(); ++e) {
        starts[e] = r;
        alignment.textAt[r] = e < elements.size() ? elements[e].begin : text.size();
        if (e < elements.size()) r += taken[e * (n + 1) + r];
    }
    for (std::size_t e = 1; e < elements.size(); ++e) {
        if (elements[e].kind != Kind::Content || elements[e - 1].kind != Kind::Kana) continue;
        std::size_t s = e - 1;
        while (s > 0 && elements[s - 1].kind == Kind::Kana) --s;
        const wchar_t last = elements[e - 1].literal;
        std::size_t at = starts[e];
        if (last == L'お' || last == L'ご') {
            // An honorific prefix goes with its word.
            if (e - 1 > s) {
                at = starts[e - 1];
            } else if (s > 0 && elements[s - 1].kind == Kind::Content) {
                at = starts[s];
            } else {
                continue;
            }
        }
        if (at > 0 && (alignment.breaks.empty() || alignment.breaks.back() < at)) alignment.breaks.push_back(at);
    }
    return alignment;
}

std::optional<Alignment> Align(const JapaneseDictionary& dictionary, std::wstring_view reading,
                               std::wstring_view text) {
    if (auto alignment = Align(dictionary, reading, text, false)) return alignment;
    return Align(dictionary, reading, text, true);
}

// The sentence's first acceptable text that lines up with the reading.
struct Aligned {
    std::size_t expected{0};
    Alignment alignment;
};
std::optional<Aligned> AlignAny(const JapaneseDictionary& dictionary, std::wstring_view reading,
                                const std::vector<std::wstring>& expected) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (auto alignment = Align(dictionary, reading, expected[i])) return Aligned{i, std::move(*alignment)};
    }
    return std::nullopt;
}

// Leaves out the breaks inside a word the dictionary writes as the text
// does there: a typist marks 戻り値 as one word, not 戻り|値.
void DropBreaksInWords(const JapaneseDictionary& dictionary, std::wstring_view reading, std::wstring_view text,
                       Alignment& alignment) {
    const auto codes = dictionary.Encode(reading);
    std::vector<char> inside(reading.size() + 1, 0);
    for (std::size_t x = 0; x < reading.size(); ++x) {
        if (alignment.textAt[x] == npos) continue;
        dictionary.CommonPrefixSearch(
            tekito::japanese::ReadingCodesView(codes).substr(x), [&](std::size_t length, std::uint32_t record) {
                const std::size_t y = x + length;
                if (length < 2 || alignment.textAt[y] == npos) return;
                const auto want = text.substr(alignment.textAt[x], alignment.textAt[y] - alignment.textAt[x]);
                bool found = false;
                dictionary.ForEachWord(record, [&](const auto& word) {
                    if (!found && dictionary.Surface(word, reading.substr(x, length)) == want) found = true;
                });
                if (!found) return;
                for (std::size_t p = x + 1; p < y; ++p) inside[p] = 1;
            });
    }
    std::erase_if(alignment.breaks, [&](std::size_t b) { return inside[b] != 0; });
}

std::wstring FirstChoice(const std::vector<Phrase>& phrases) {
    std::wstring text;
    for (const auto& phrase : phrases) {
        if (!phrase.candidates.empty()) text += phrase.candidates.front().text;
    }
    return text;
}

// The kana `keys` make, as the composer reads them.
std::wstring ReadingOf(JapaneseComposer& composer, std::wstring_view keys) {
    composer.Clear();
    for (const wchar_t key : keys) composer.Insert(key);
    composer.Convert();
    composer.Cancel();
    auto reading = composer.TypedReading();
    composer.Clear();
    return reading;
}

// The composer's context after a commit: what it keeps for the next
// conversion (JapaneseComposer::Commit).
struct Context {
    std::uint16_t rightId{0};
    std::vector<std::wstring> words;

    void After(std::wstring_view text, std::uint16_t right) {
        rightId = text.empty() || std::wstring_view(L"。．.！!？?\n").find(text.back()) != std::wstring_view::npos
                      ? 0
                      : right;
        const auto content = [](wchar_t c) {
            return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005 || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC;
        };
        for (std::size_t i = 0; i < text.size();) {
            if (!content(text[i])) {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < text.size() && content(text[end])) ++end;
            if (end - i >= 2) words.emplace_back(text.substr(i, end - i));
            i = end;
        }
        if (words.size() > 8) words.erase(words.begin(), words.end() - 8);
    }
};

struct Score {
    std::size_t rows{0}, exact{0}, errors{0}, characters{0};
    void Add(std::wstring_view output, const std::vector<std::wstring>& expected) {
        const auto [distance, length] = ja_eval::Closest(output, expected);
        ++rows;
        if (distance == 0) ++exact;
        errors += distance;
        characters += length;
    }
    [[nodiscard]] std::string Text() const {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << "top1=" << (rows ? 100.0 * exact / rows : 0.0)
            << "% cer=" << (characters ? 100.0 * errors / characters : 0.0) << "%";
        return out.str();
    }
};

std::string Latency(const std::vector<double>& us) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << "p50=" << Percentile(us, 0.5) << " p95=" << Percentile(us, 0.95)
        << " p99=" << Percentile(us, 0.99) << " max=" << Percentile(us, 1.0) << " us";
    return out.str();
}

bool Right(std::wstring_view output, const std::vector<std::wstring>& expected) {
    return std::find(expected.begin(), expected.end(), output) != expected.end();
}

// ---------------------------------------------------------------------------
// Phrase breaks marked with Space.

int RunHints(const JapaneseDictionary& dictionary, const JapaneseConverter& converter, JapaneseComposer& composer,
             const std::vector<ja_eval::Row>& rows, std::size_t showExamples) {
    struct Totals {
        std::size_t rows{0}, aligned{0}, withBreaks{0}, breaks{0}, agree{0}, inWords{0};
        Score whole, commit, hints, fixed, commitHalf, hintsHalf;
        std::size_t hintGain{0}, hintLoss{0}, commitGain{0}, commitLoss{0};
        std::vector<double> commitUs, hintUs, wholeUs;
    };
    std::map<std::string, Totals> totals;
    std::size_t shown = 0;
    for (const auto& row : rows) {
        const std::wstring reading = ReadingOf(composer, row.reading);
        auto aligned = AlignAny(dictionary, reading, row.expected);
        for (Totals* t : {&totals[row.source], &totals["all"]}) ++t->rows;
        if (!aligned) continue;
        const std::size_t orthographic = aligned->alignment.breaks.size();
        DropBreaksInWords(dictionary, reading, row.expected[aligned->expected], aligned->alignment);
        const auto& breaks = aligned->alignment.breaks;
        // Half the breaks, the same ones every run: a typist who marks some.
        std::vector<std::size_t> half;
        for (std::size_t i = 0; i < breaks.size(); ++i) {
            if ((std::hash<std::string>{}(row.id) + i) % 2 == 0) half.push_back(breaks[i]);
        }

        auto start = Clock::now();
        const auto wholePhrases = converter.Convert(reading);
        const double wholeUs = Since(start);
        const std::wstring whole = FirstChoice(wholePhrases);
        // Whether the conversion already breaks everywhere the typist would.
        std::set<std::size_t> starts;
        for (const auto& phrase : wholePhrases) starts.insert(phrase.begin);
        const bool agree = std::all_of(breaks.begin(), breaks.end(), [&](std::size_t b) { return starts.count(b); });

        // Today: Space converts what was typed since the last one, and the
        // next key commits it.
        std::vector<double> commitUs;
        const auto typeCommitting = [&](const std::vector<std::size_t>& at, std::vector<double>* us) {
            Context context;
            std::wstring text;
            std::size_t begin = 0;
            for (std::size_t i = 0; i <= at.size(); ++i) {
                const std::size_t end = i < at.size() ? at[i] : reading.size();
                const auto piece = std::wstring_view(reading).substr(begin, end - begin);
                const auto pieceStart = Clock::now();
                const auto phrases = converter.Convert(piece, {}, context.rightId, context.words);
                if (us) us->push_back(Since(pieceStart));
                const std::wstring chosen = FirstChoice(phrases);
                const std::uint16_t right = phrases.empty() || phrases.back().candidates.empty()
                                                ? 0
                                                : phrases.back().candidates.front().rightId;
                context.After(chosen, right);
                text += chosen;
                begin = end;
            }
            return text;
        };
        const std::wstring commit = typeCommitting(breaks, &commitUs);
        const std::wstring commitHalf = typeCommitting(half, nullptr);

        // As hints: each Space shows the text so far converted with the
        // breaks kept; the last converts it all.
        std::vector<double> hintUs;
        for (std::size_t i = 0; i < breaks.size(); ++i) {
            const auto hintStart = Clock::now();
            (void)converter.Convert(std::wstring_view(reading).substr(0, breaks[i]), {}, 0, {}, nullptr,
                                    std::span(breaks.data(), i));
            hintUs.push_back(Since(hintStart));
        }
        start = Clock::now();
        const std::wstring hints = FirstChoice(converter.Convert(reading, {}, 0, {}, nullptr, breaks));
        hintUs.push_back(Since(start));
        const std::wstring hintsHalf = FirstChoice(converter.Convert(reading, {}, 0, {}, nullptr, half));
        // The breaks as fixed phrase lengths: nothing split between them.
        std::vector<std::size_t> lengths;
        for (std::size_t i = 0, begin = 0; i < breaks.size(); ++i) {
            lengths.push_back(breaks[i] - begin);
            begin = breaks[i];
        }
        const std::wstring fixed = FirstChoice(converter.Convert(reading, lengths));

        const bool wholeRight = Right(whole, row.expected);
        for (Totals* t : {&totals[row.source], &totals["all"]}) {
            ++t->aligned;
            if (!breaks.empty()) ++t->withBreaks;
            t->breaks += breaks.size();
            t->inWords += orthographic - breaks.size();
            if (agree) ++t->agree;
            t->whole.Add(whole, row.expected);
            t->commit.Add(commit, row.expected);
            t->hints.Add(hints, row.expected);
            t->fixed.Add(fixed, row.expected);
            t->commitHalf.Add(commitHalf, row.expected);
            t->hintsHalf.Add(hintsHalf, row.expected);
            if (Right(hints, row.expected) && !wholeRight) ++t->hintGain;
            if (!Right(hints, row.expected) && wholeRight) ++t->hintLoss;
            if (Right(commit, row.expected) && !wholeRight) ++t->commitGain;
            if (!Right(commit, row.expected) && wholeRight) ++t->commitLoss;
            t->commitUs.insert(t->commitUs.end(), commitUs.begin(), commitUs.end());
            t->hintUs.insert(t->hintUs.end(), hintUs.begin(), hintUs.end());
            t->wholeUs.push_back(wholeUs);
        }
        if (shown < showExamples && hints != whole && (Right(hints, row.expected) || wholeRight)) {
            ++shown;
            std::wstring marked = reading;
            for (auto b = breaks.rbegin(); b != breaks.rend(); ++b) marked.insert(*b, L"|");
            std::cout << (Right(hints, row.expected) ? "gain " : "loss ") << row.source << ":" << row.id << "  "
                      << Narrow(marked) << "  whole " << Narrow(whole) << "  hints " << Narrow(hints) << "\n";
        }
    }
    std::cout << std::fixed << std::setprecision(1);
    for (const auto& [source, t] : totals) {
        std::cout << source << ": rows=" << t.rows << " aligned=" << t.aligned << " with_breaks=" << t.withBreaks
                  << " breaks_per_sentence=" << (t.aligned ? double(t.breaks) / t.aligned : 0.0)
                  << " (left out inside words: " << t.inWords << ")"
                  << " conversion_already_breaks_there=" << (t.aligned ? 100.0 * t.agree / t.aligned : 0.0) << "%\n"
                  << "  whole (Space at the end)       " << t.whole.Text() << "\n"
                  << "  commit at each Space (today)   " << t.commit.Text() << "  gain=" << t.commitGain
                  << " loss=" << t.commitLoss << "\n"
                  << "  breaks as hints                " << t.hints.Text() << "  gain=" << t.hintGain
                  << " loss=" << t.hintLoss << "\n"
                  << "  breaks as fixed phrases        " << t.fixed.Text() << "\n"
                  << "  half the breaks, commit        " << t.commitHalf.Text() << "\n"
                  << "  half the breaks, hints         " << t.hintsHalf.Text() << "\n"
                  << "  Space latency: commit " << Latency(t.commitUs) << "; hints " << Latency(t.hintUs) << "; whole "
                  << Latency(t.wholeUs) << "\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Live conversion, with phrases settled early.

int RunLive(const JapaneseConverter& converter, JapaneseComposer& composer, const std::vector<ja_eval::Row>& rows,
            std::size_t showExamples) {
    // 0: Space conversion as today (only the romaji on each key); -1: live
    // with Best (no phrases, no model); otherwise live with this many
    // phrases kept open (a huge number: none settled).
    constexpr long long kAll = 1 << 20;
    const std::vector<long long> modes{0, -1, kAll, 4, 3, 2, 1};
    const auto name = [&](long long mode) -> std::string {
        if (mode == 0) return "space conversion (today)";
        if (mode == -1) return "live, first choice only (Best)";
        if (mode == kAll) return "live, nothing settled";
        return "live, keep " + std::to_string(mode) + " phrases open";
    };
    std::size_t maxReading = 0;
    for (const long long mode : modes) {
        struct Totals {
            Score score;
            std::vector<double> keyUs, spaceUs;
            std::size_t keys{0}, rewrites{0}, deepRewrites{0}, sentencesDeep{0}, sentences{0};
        } t;
        std::size_t shown = 0;
        for (const auto& row : rows) {
            composer.Clear();
            Context context;
            std::wstring frozen;
            std::size_t frozenLength = 0;
            std::wstring lastReading;
            std::vector<Phrase> open;
            std::wstring display;
            std::vector<std::size_t> openStarts;  // in the display
            std::size_t deep = 0;
            const auto convertOpen = [&](const std::wstring& reading) {
                open = converter.Convert(std::wstring_view(reading).substr(frozenLength), {}, context.rightId,
                                         context.words);
                if (mode > 0 && open.size() > static_cast<std::size_t>(mode)) {
                    const std::size_t settle = open.size() - static_cast<std::size_t>(mode);
                    for (std::size_t i = 0; i < settle; ++i) {
                        const auto& phrase = open[i];
                        const auto& first = phrase.candidates.front();
                        frozen += first.text;
                        frozenLength += phrase.length;
                        context.After(first.text, first.rightId);
                    }
                    open.erase(open.begin(), open.begin() + static_cast<std::ptrdiff_t>(settle));
                }
            };
            for (const wchar_t key : row.reading) {
                const auto start = Clock::now();
                composer.Insert(key);
                if (mode != 0) {
                    const std::wstring reading = composer.TypedReading();
                    if (reading != lastReading && reading.size() > frozenLength) {
                        lastReading = reading;
                        std::wstring next;
                        std::vector<std::size_t> starts;
                        if (mode == -1) {
                            const auto best = converter.Best(reading);
                            next = best ? best->text : reading;
                        } else {
                            convertOpen(reading);
                            next = frozen;
                            for (const auto& phrase : open) {
                                starts.push_back(next.size());
                                next += phrase.candidates.empty() ? std::wstring() : phrase.candidates.front().text;
                            }
                        }
                        const double us = Since(start);
                        t.keyUs.push_back(us);
                        // What changed before the phrase being typed.
                        std::size_t same = 0;
                        while (same < display.size() && same < next.size() && display[same] == next[same]) ++same;
                        if (!openStarts.empty() && same < openStarts.back()) ++t.rewrites;
                        if (openStarts.size() >= 2 && same < openStarts[openStarts.size() - 2]) {
                            ++t.deepRewrites;
                            ++deep;
                        }
                        display = std::move(next);
                        openStarts = std::move(starts);
                        ++t.keys;
                        continue;
                    }
                }
                t.keyUs.push_back(Since(start));
                ++t.keys;
            }
            // Enter (live) or Space (today): the whole text converted.
            composer.Convert();
            composer.Cancel();
            const std::wstring reading = composer.TypedReading();
            maxReading = std::max(maxReading, reading.size());
            std::wstring output;
            const auto start = Clock::now();
            if (mode == 0 || mode == kAll) {
                output = FirstChoice(converter.Convert(reading));
            } else if (mode == -1) {
                const auto best = converter.Best(reading);
                output = best ? best->text : reading;
            } else {
                convertOpen(reading);
                output = frozen + FirstChoice(open);
            }
            t.spaceUs.push_back(Since(start));
            t.score.Add(output, row.expected);
            ++t.sentences;
            if (deep) ++t.sentencesDeep;
            if (shown < showExamples && deep) {
                ++shown;
                std::cout << "rewrites " << row.source << ":" << row.id << " " << deep << "  " << Narrow(output)
                          << "\n";
            }
        }
        std::cout << std::fixed << std::setprecision(1) << name(mode) << ": " << t.score.Text() << "  per key "
                  << Latency(t.keyUs) << "  last conversion " << Latency(t.spaceUs);
        if (mode == -1) std::cout << "  rewrites n/a (no phrases)";
        if (mode > 0) {
            std::cout << "  rewrites_per_100_keys=" << 100.0 * t.rewrites / t.keys
                      << " two_back_per_100_keys=" << 100.0 * t.deepRewrites / t.keys
                      << " sentences_with_two_back=" << 100.0 * t.sentencesDeep / t.sentences << "%";
        }
        std::cout << "\n";
    }
    std::cout << "longest reading " << maxReading << " kana\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Words registered when no candidate fits.

int RunRegister(const JapaneseDictionary& dictionary, const JapaneseConverter& converter, JapaneseComposer& composer,
                const std::vector<ja_eval::Row>& rows, const std::filesystem::path& pack, std::size_t showExamples) {
    const auto parts = tekito::japanese::UserPartsOfSpeech::Load(pack / "pos.tsv");
    if (parts.Empty()) {
        std::cerr << "no pos.tsv in " << pack.string() << "\n";
        return 1;
    }
    struct Sentence {
        std::wstring reading;
        std::optional<Aligned> aligned;
        bool right{false};
    };
    std::vector<Sentence> sentences;
    sentences.reserve(rows.size());
    Score baseline;
    for (const auto& row : rows) {
        Sentence s;
        s.reading = ReadingOf(composer, row.reading);
        s.aligned = AlignAny(dictionary, s.reading, row.expected);
        const auto output = FirstChoice(converter.Convert(s.reading));
        s.right = Right(output, row.expected);
        baseline.Add(output, row.expected);
        sentences.push_back(std::move(s));
    }

    // Whether `text` can be put together from shorter phrases of `reading`,
    // each among its first nine candidates.
    const auto buildable = [&](std::wstring_view reading, const std::vector<std::size_t>& textAt, std::size_t begin,
                               std::size_t end, std::wstring_view text) {
        std::vector<char> reached(end + 1, 0);
        reached[begin] = 1;
        for (std::size_t x = begin; x < end; ++x) {
            if (!reached[x] || textAt[x] == npos) continue;
            for (std::size_t y = x + 1; y <= end; ++y) {
                if (textAt[y] == npos || (x == begin && y == end)) continue;
                const auto sub = reading.substr(x, y - x);
                const std::size_t length = y - x;
                const auto phrases = converter.Convert(sub, std::span(&length, 1));
                if (phrases.empty()) continue;
                const auto want = text.substr(textAt[x], textAt[y] - textAt[x]);
                const auto& candidates = phrases.front().candidates;
                for (std::size_t i = 0; i < candidates.size() && i < 9; ++i) {
                    if (candidates[i].text == want) {
                        reached[y] = 1;
                        break;
                    }
                }
            }
        }
        return reached[end] != 0;
    };

    std::vector<tekito::japanese::UserWord> words;
    JapaneseUserDictionary user;
    std::size_t wrong = 0, unaligned = 0, missSentences = 0, inListOnly = 0, noFitSentences = 0, noFit = 0,
                noFitBeyond9 = 0, built = 0, gains = 0, losses = 0, shown = 0;
    Score online;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        const auto& s = sentences[r];
        const auto phrases = converter.Convert(s.reading, {}, 0, {}, user.Empty() ? nullptr : &user);
        const auto output = FirstChoice(phrases);
        const bool right = Right(output, row.expected);
        if (right && !s.right) ++gains;
        if (!right && s.right) ++losses;
        online.Add(output, row.expected);
        if (right) continue;
        ++wrong;
        if (!s.aligned) {
            ++unaligned;
            continue;
        }
        const auto& text = row.expected[s.aligned->expected];
        const auto& textAt = s.aligned->alignment.textAt;
        bool missed = false, anyNoFit = false;
        for (const auto& phrase : phrases) {
            const std::size_t begin = phrase.begin, end = phrase.begin + phrase.length;
            if (textAt[begin] == npos || textAt[end] == npos || phrase.candidates.empty()) continue;
            const auto want = text.substr(textAt[begin], textAt[end] - textAt[begin]);
            if (phrase.candidates.front().text == want) continue;
            missed = true;
            const auto found = std::find_if(phrase.candidates.begin(), phrase.candidates.end(),
                                            [&](const auto& c) { return c.text == want; });
            if (found != phrase.candidates.end()) {
                if (found - phrase.candidates.begin() >= 9) ++noFitBeyond9;
                continue;
            }
            anyNoFit = true;
            ++noFit;
            const auto reading = s.reading.substr(begin, phrase.length);
            if (buildable(s.reading, textAt, begin, end, text)) ++built;
            if (shown < showExamples) {
                ++shown;
                std::cout << "no fit " << row.source << ":" << row.id << "  " << Narrow(reading) << " -> "
                          << Narrow(want) << "  (first " << Narrow(phrase.candidates.front().text) << ")\n";
            }
            words.push_back(
                {reading, want, tekito::japanese::UserWordKind::Noun, tekito::japanese::UserWordAction::First});
        }
        if (missed) ++missSentences;
        if (missed && !anyNoFit) ++inListOnly;
        if (anyNoFit) {
            ++noFitSentences;
            user.Set(words, parts);
        }
    }

    // Everything registered from the start: what the words do to the rest.
    Score all;
    std::size_t allGains = 0, allLosses = 0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto output = FirstChoice(converter.Convert(sentences[r].reading, {}, 0, {}, &user));
        const bool right = Right(output, rows[r].expected);
        if (right && !sentences[r].right) ++allGains;
        if (!right && sentences[r].right) ++allLosses;
        all.Add(output, rows[r].expected);
    }

    std::cout << std::fixed << std::setprecision(1) << "rows=" << rows.size() << " baseline " << baseline.Text()
              << "\n  wrong (online)=" << wrong << " unaligned=" << unaligned
              << " with a wrong phrase=" << missSentences << " (all in the list=" << inListOnly
              << ", a phrase no candidate fits=" << noFitSentences << ")\n"
              << "  phrases no candidate fits=" << noFit << " (in the list but past 9: " << noFitBeyond9 << ")"
              << "  can be put together from shorter phrases=" << (noFit ? 100.0 * built / noFit : 0.0) << "%\n"
              << "  registering as they come: " << online.Text() << " gain=" << gains << " loss=" << losses
              << " (sentences after a registration)\n"
              << "  all registered from the start: " << all.Text() << " gain=" << allGains << " loss=" << allLosses
              << "\n  registered words=" << words.size() << "\n";
    return 0;
}

}  // namespace

int RunTypingStudy(const JapaneseDictionary& dictionary, const JapaneseConverter& converter,
                   const std::filesystem::path& corpus, const std::filesystem::path& romaji,
                   const TypingStudyOptions& options) {
    const auto table = tekito::japanese::RomajiTable::Load(romaji);
    if (!table) {
        std::cerr << "could not open the romaji table\n";
        return 1;
    }
    // Only the romaji: conversions are made here, as each way would.
    JapaneseComposer composer(table.get());
    auto rows = ja_eval::LoadCorpus(corpus);
    if (options.join > 1) {
        std::vector<ja_eval::Row> joined;
        for (std::size_t i = 0; i + options.join <= rows.size(); i += options.join) {
            ja_eval::Row row{rows[i].source, rows[i].id, {}, {std::wstring()}};
            for (std::size_t j = i; j < i + options.join; ++j) {
                row.reading += rows[j].reading;
                row.expected.front() += rows[j].expected.front();
            }
            joined.push_back(std::move(row));
        }
        rows = std::move(joined);
    }
    if (options.sample > 0 && rows.size() > options.sample) {
        // Every so many rows, so each source keeps its share.
        std::vector<ja_eval::Row> sampled;
        const double step = static_cast<double>(rows.size()) / static_cast<double>(options.sample);
        for (std::size_t i = 0; i < options.sample; ++i) sampled.push_back(rows[static_cast<std::size_t>(i * step)]);
        rows = std::move(sampled);
    }
    if (options.study == "hints") return RunHints(dictionary, converter, composer, rows, options.showExamples);
    if (options.study == "live") return RunLive(converter, composer, rows, options.showExamples);
    if (options.study == "register")
        return RunRegister(dictionary, converter, composer, rows, options.pack, options.showExamples);
    std::cerr << "--study takes hints, live or register\n";
    return 2;
}
