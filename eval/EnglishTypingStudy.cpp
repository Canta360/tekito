#include "EnglishTypingStudy.h"

#include "Core/ConversionEngine.h"
#include "Core/InputStateMachine.h"
#include "Core/UserDictionary.h"
#include "Core/UserLearning.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::wstring Widen(std::string_view utf8) {
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string Narrow(std::wstring_view text) {
#if defined(_WIN32)
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
#else
    return std::string(text.begin(), text.end());
#endif
}

double Percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<std::size_t>(q * values.size()))];
}

// As the text service composes: letters and the apostrophe of contractions.
bool IsWordCharacter(wchar_t c) { return std::iswalpha(c) != 0 || c == L'\'' || c == L'\u2019'; }
bool IsQuote(wchar_t c) { return c == L'\'' || c == L'\u2019' || c == L'\u2018'; }

// A word TEKITO composes: after whitespace, a bracket, a quote or a dash,
// and ended by whitespace or punctuation. Words touching digits, slashes,
// @ and the like are left as typed by the text service and not counted.
struct Word {
    std::size_t begin;
    std::wstring text;
};
std::vector<Word> Words(std::wstring_view sentence) {
    const std::wstring_view before = L" \t([{\"\u201C\u2018'\u2014\u2013-";
    const std::wstring_view after = L" \t.,;:!?)]}\"\u201D\u2019'\u2014\u2013-";
    std::vector<Word> words;
    for (std::size_t i = 0; i < sentence.size();) {
        if (!IsWordCharacter(sentence[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < sentence.size() && IsWordCharacter(sentence[end])) ++end;
        std::size_t b = i, e = end;
        while (b < e && IsQuote(sentence[b])) ++b;
        while (e > b && IsQuote(sentence[e - 1])) --e;
        const bool startOk = b == 0 || before.find(sentence[b - 1]) != std::wstring_view::npos;
        const bool endOk = e == sentence.size() || after.find(sentence[e]) != std::wstring_view::npos;
        if (e > b && startOk && endOk) words.push_back({b, std::wstring(sentence.substr(b, e - b))});
        i = end;
    }
    return words;
}

std::string Kind(std::wstring_view word, bool sentenceStart) {
    const bool allUpper = word.size() > 1 && std::all_of(word.begin(), word.end(), [](wchar_t c) {
                              return !std::iswalpha(c) || std::iswupper(c);
                          });
    if (allUpper) return "ALL CAPS";
    if (std::iswupper(word.front()) && !sentenceStart) return "Capitalized";
    if (std::any_of(word.begin(), word.end(), [](wchar_t c) { return c == L'\'' || c == L'\u2019'; })) {
        return "apostrophe";
    }
    if (std::iswupper(word.front())) return "sentence start";
    return "lowercase";
}

struct Sentence {
    std::wstring text;
};

// People type the straight apostrophe; the news has curly ones.
std::wstring AsTyped(std::wstring text) {
    for (auto& c : text) {
        if (c == static_cast<wchar_t>(0x2019) || c == static_cast<wchar_t>(0x2018)) c = L'\'';
    }
    return text;
}

std::vector<Sentence> LoadSentences(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab != std::string::npos) lines.push_back(line.substr(tab + 1));
    }
    std::vector<Sentence> sentences;
    const std::size_t count = std::min(limit, lines.size());
    const double step = count ? static_cast<double>(lines.size()) / static_cast<double>(count) : 1.0;
    for (std::size_t i = 0; i < count; ++i) {
        sentences.push_back({AsTyped(Widen(lines[static_cast<std::size_t>(i * step)]))});
    }
    return sentences;
}

// Today; keeping a word once its correction is undone (user dictionary);
// deciding only once the next word is typed (with it as the text that
// follows).
enum class Mode { Today, KeepOnUndo, WaitForNext };

const char* Name(Mode mode) {
    switch (mode) {
        case Mode::Today: return "today";
        case Mode::KeepOnUndo: return "keep a word once undone";
        case Mode::WaitForNext: return "decide at the next word";
    }
    return "";
}

// The request the text service makes for `typed` in place of `word`.
tekito::ConversionRequest RequestFor(std::wstring_view sentence, const Word& word, std::wstring_view typed,
                                     Mode mode = Mode::Today) {
    constexpr std::size_t kContext = 128;  // as TextService reads it
    tekito::ConversionRequest request;
    const std::size_t from = word.begin > kContext ? word.begin - kContext : 0;
    request.context.precedingText = std::wstring(sentence.substr(from, word.begin - from));
    request.context.sentenceStart = tekito::IsSentenceStart(sentence.substr(0, word.begin));
    request.rawText = std::wstring(typed);
    // At a sentence start TEKITO capitalized the letter typed in lower case.
    const bool engineCapital = request.context.sentenceStart && std::iswupper(typed.front()) &&
                               (typed.size() == 1 || !std::iswupper(typed[1]));
    request.capitalization.origin =
        engineCapital ? tekito::CapitalizationOrigin::EngineApplied : tekito::CapitalizationOrigin::UserTyped;
    if (mode == Mode::WaitForNext) {
        // The text up to the end of the next word.
        std::size_t end = word.begin + word.text.size();
        while (end < sentence.size() && !IsWordCharacter(sentence[end])) ++end;
        while (end < sentence.size() && IsWordCharacter(sentence[end])) ++end;
        request.context.followingText = std::wstring(sentence.substr(word.begin + word.text.size(),
                                                                     end - word.begin - word.text.size()));
    }
    return request;
}

// What Space leaves for `request`: the text, without the space.
std::wstring AfterSpace(tekito::InputStateMachine& state, const std::wstring& typed,
                        const std::vector<tekito::Candidate>& candidates) {
    state.BeginOrUpdate(typed, candidates);
    const auto action = state.OnSpace();
    state.Reset();
    if (action.kind != tekito::ActionKind::ReplaceComposition || action.text.empty()) return typed;
    return action.text.substr(0, action.text.size() - 1);
}

// One word per sentence typed as a real word a letter away (the -> them),
// to see whether Space brings the meant word back.
struct Slip {
    std::size_t sentence;
    std::size_t word;
    std::wstring typed;
};

std::vector<Slip> MakeSlips(const std::vector<Sentence>& sentences) {
    tekito::UserDictionary none;
    auto engine = tekito::CreateDefaultConversionEngine(none);
    std::vector<Slip> slips;
    for (std::size_t s = 0; s < sentences.size(); ++s) {
        const auto words = Words(sentences[s].text);
        if (words.empty()) continue;
        // A word chosen by position, the same every run.
        for (std::size_t k = 0; k < words.size(); ++k) {
            const auto& word = words[(s * 7 + k) % words.size()];
            if (word.text.size() < 3) continue;
            const auto candidates = engine->Convert(RequestFor(sentences[s].text, word, word.text)).candidates;
            const auto slip = std::find_if(candidates.begin(), candidates.end(), [&](const tekito::Candidate& c) {
                return !c.isOriginal && (c.sourceFlags & tekito::CandidateSourceSingleEdit) != 0 &&
                       (c.sourceFlags & tekito::CandidateSourceExternalLexicon) != 0 && c.text.size() >= 3 &&
                       std::all_of(c.text.begin(), c.text.end(), [](wchar_t ch) { return std::iswalpha(ch) != 0; });
            });
            if (slip == candidates.end()) continue;
            slips.push_back({s, (s * 7 + k) % words.size(), slip->text});
            break;
        }
    }
    return slips;
}

struct RunResult {
    std::size_t words{0}, rewrites{0}, afterUndo{0}, sentencesHit{0};
    std::size_t slips{0}, slipsFixed{0};
    std::map<std::string, std::size_t> byKind;
    std::map<std::string, std::size_t> kindWords;
    std::map<std::pair<std::wstring, std::wstring>, std::size_t> pairs;
    std::map<std::pair<std::wstring, std::wstring>, std::size_t> repeats;
    std::vector<double> convertUs;
    std::vector<std::wstring> kept;
};

RunResult Run(Mode mode, const std::vector<Sentence>& sentences, const std::vector<Slip>& slips) {
    tekito::UserDictionary dictionary;
    tekito::UserLearningStore store;
    tekito::UserLearningProvider provider(store);
    auto engine = tekito::CreateDefaultConversionEngine(dictionary, provider);
    tekito::InputStateMachine state;
    state.SetInputMode(tekito::InputMode::Convert);

    RunResult result;
    // Words already rewritten once: from then on, every decision about them
    // is recorded as the text service records it (recording every word's
    // would make the store too big to try quickly).
    std::set<std::wstring> undone;
    tekito::UserDictionaryEntryId nextId = 1;
    for (const auto& sentence : sentences) {
        bool hit = false;
        for (const auto& word : Words(sentence.text)) {
            const auto request = RequestFor(sentence.text, word, word.text, mode);
            const auto start = Clock::now();
            const auto candidates = engine->Convert(request).candidates;
            result.convertUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
            const auto after = AfterSpace(state, word.text, candidates);
            ++result.words;
            const std::string kind = Kind(word.text, request.context.sentenceStart);
            ++result.kindWords[kind];
            if (after == word.text) {
                if (undone.count(word.text)) {
                    // Kept: Space leaves it, the next key commits it.
                    for (const auto& candidate : candidates) {
                        if (candidate.text.empty()) continue;
                        store.RecordExposure(word.text, candidate.text);
                        if (candidate.text != word.text) store.RecordNonSelection(word.text, candidate.text);
                    }
                    store.RecordAutomaticAcceptance(word.text, word.text);
                    store.RecordRawKeep(word.text);
                }
                continue;
            }
            ++result.rewrites;
            ++result.byKind[kind];
            ++result.pairs[{word.text, after}];
            hit = true;
            if (undone.count(word.text)) {
                ++result.afterUndo;
                ++result.repeats[{word.text, after}];
            }
            undone.insert(word.text);
            // Backspace brings the word back (RestoreOriginal), and it is kept.
            store.RecordUndo(after);
            store.RecordExposure(word.text, after);
            store.RecordUndo(word.text, after);
            store.RecordRawKeep(word.text);
            if (mode == Mode::KeepOnUndo) {
                tekito::UserDictionaryEntry keep;
                keep.id = nextId++;
                keep.raw = word.text;
                keep.candidate = word.text;
                keep.policyFlags = tekito::CandidatePolicyProtect;
                if (dictionary.Add(std::move(keep))) {
                    engine->RefreshUserDictionary();
                    result.kept.push_back(word.text);
                }
            }
        }
        if (hit) ++result.sentencesHit;
    }
    // Real words typed by mistake, with a fresh profile.
    tekito::UserDictionary none;
    auto fresh = tekito::CreateDefaultConversionEngine(none);
    for (const auto& slip : slips) {
        const auto& text = sentences[slip.sentence].text;
        const auto word = Words(text)[slip.word];
        const auto candidates = fresh->Convert(RequestFor(text, word, slip.typed, mode)).candidates;
        ++result.slips;
        if (AfterSpace(state, slip.typed, candidates) == word.text) ++result.slipsFixed;
    }
    return result;
}

// Per key, as the text service converts on every key of a word.
std::vector<double> KeyLatency(const std::vector<Sentence>& sentences, std::size_t count) {
    tekito::UserDictionary none;
    auto engine = tekito::CreateDefaultConversionEngine(none);
    std::vector<double> us;
    for (std::size_t s = 0; s < sentences.size() && s < count; ++s) {
        for (const auto& word : Words(sentences[s].text)) {
            for (std::size_t length = 1; length <= word.text.size(); ++length) {
                const auto request = RequestFor(sentences[s].text, word, word.text.substr(0, length));
                const auto start = Clock::now();
                (void)engine->Convert(request);
                us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
            }
        }
    }
    return us;
}

std::vector<std::pair<std::wstring, std::wstring>> LoadTypos(const std::filesystem::path& corpus) {
    std::vector<std::pair<std::wstring, std::wstring>> typos;
    std::ifstream stream(corpus, std::ios::binary);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> fields;
        std::stringstream parts(line);
        for (std::string field; std::getline(parts, field, '\t');) fields.push_back(field);
        if (fields.size() < 7 || fields[4] != "auto_apply_eligible") continue;
        typos.emplace_back(Widen(fields[5]), Widen(fields[6]));
    }
    return typos;
}

void PrintTop(const std::map<std::pair<std::wstring, std::wstring>, std::size_t>& pairs, std::size_t count) {
    std::vector<std::pair<std::size_t, std::pair<std::wstring, std::wstring>>> top;
    for (const auto& [pair, n] : pairs) top.push_back({n, pair});
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < top.size() && i < count; ++i) {
        std::cout << "    " << top[i].first << "x  " << Narrow(top[i].second.first) << " -> "
                  << Narrow(top[i].second.second) << "\n";
    }
}

}  // namespace

int RunEnglishStudy(const EnglishStudyOptions& options) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    tekito::PreloadDefaultEngineData();
    if (!options.probe.empty()) {
        // Each word of one sentence: what Space does and the first candidates.
        tekito::UserDictionary dictionary;
        auto engine = tekito::CreateDefaultConversionEngine(dictionary);
        tekito::InputStateMachine state;
        state.SetInputMode(tekito::InputMode::Convert);
        const auto sentence = AsTyped(Widen(options.probe));
        for (const auto& word : Words(sentence)) {
            const auto candidates = engine->Convert(RequestFor(sentence, word, word.text)).candidates;
            std::cout << Narrow(word.text) << " -> " << Narrow(AfterSpace(state, word.text, candidates)) << " |";
            for (std::size_t i = 0; i < candidates.size() && i < 4; ++i) {
                const auto& c = candidates[i];
                std::cout << "  [" << Narrow(c.text) << " label=" << static_cast<int>(c.label)
                          << " orig=" << c.isOriginal << " policy=0x" << std::hex << c.policyFlags
                          << " source=0x" << c.sourceFlags << std::dec << "]";
            }
            std::cout << "\n";
        }
        return 0;
    }

    const auto sentences = LoadSentences(options.sentences, options.maxSentences);
    if (sentences.empty()) {
        std::cerr << "no sentences in " << options.sentences.string() << "\n";
        return 2;
    }
    const auto slips = MakeSlips(sentences);
    std::cout << std::fixed << std::setprecision(2) << "sentences=" << sentences.size()
              << " slips (a real word a letter away typed instead)=" << slips.size() << "\n";
    const auto keys = KeyLatency(sentences, 300);
    std::cout << "per key (every key of a word converts): p50=" << Percentile(keys, 0.5)
              << " p95=" << Percentile(keys, 0.95) << " p99=" << Percentile(keys, 0.99) << " us\n";

    std::vector<std::wstring> kept;
    for (const Mode mode : {Mode::Today, Mode::KeepOnUndo, Mode::WaitForNext}) {
        const auto r = Run(mode, sentences, slips);
        std::cout << Name(mode) << ": words=" << r.words << " rewritten=" << r.rewrites << " ("
                  << 10000.0 * r.rewrites / r.words << " per 10k words, sentences hit "
                  << 100.0 * r.sentencesHit / sentences.size() << "%) again_after_undo=" << r.afterUndo
                  << "  slips fixed=" << r.slipsFixed << "/" << r.slips << " ("
                  << (r.slips ? 100.0 * r.slipsFixed / r.slips : 0.0) << "%)"
                  << "  word convert p50=" << Percentile(r.convertUs, 0.5)
                  << " p95=" << Percentile(r.convertUs, 0.95) << " us\n";
        for (const auto& [kind, count] : r.byKind) {
            std::cout << "    " << kind << ": " << count << " of " << r.kindWords.at(kind) << " ("
                      << 10000.0 * count / r.kindWords.at(kind) << " per 10k)\n";
        }
        if (mode == Mode::Today) PrintTop(r.pairs, options.showExamples);
        if (mode == Mode::KeepOnUndo) {
            std::cout << "  still rewritten after being kept:\n";
            PrintTop(r.repeats, 10);
            kept = r.kept;
        }
    }

    // Does keeping those words stop real corrections?
    if (!options.corpus.empty()) {
        const auto typos = LoadTypos(options.corpus);
        tekito::UserDictionary none, keeps;
        tekito::UserDictionaryEntryId id = 1;
        for (const auto& word : kept) {
            tekito::UserDictionaryEntry keep;
            keep.id = id++;
            keep.raw = word;
            keep.candidate = word;
            keep.policyFlags = tekito::CandidatePolicyProtect;
            (void)keeps.Add(std::move(keep));
        }
        std::size_t fixedBefore = 0, fixedAfter = 0, lost = 0;
        auto before = tekito::CreateDefaultConversionEngine(none);
        auto after = tekito::CreateDefaultConversionEngine(keeps);
        tekito::InputStateMachine state;
        state.SetInputMode(tekito::InputMode::Convert);
        for (const auto& [raw, want] : typos) {
            tekito::ConversionRequest request;
            request.rawText = raw;
            const bool a = AfterSpace(state, raw, before->Convert(request).candidates) == want;
            const bool b = AfterSpace(state, raw, after->Convert(request).candidates) == want;
            fixedBefore += a;
            fixedAfter += b;
            if (a && !b) {
                if (lost < 10) std::cout << "    lost: " << Narrow(raw) << " -> " << Narrow(want) << "\n";
                ++lost;
            }
        }
        std::cout << "typo rows=" << typos.size() << " corrected at Space: " << 100.0 * fixedBefore / typos.size()
                  << "% -> with the " << kept.size() << " kept words " << 100.0 * fixedAfter / typos.size()
                  << "% (lost " << lost << ")\n";
    }
    return 0;
}
