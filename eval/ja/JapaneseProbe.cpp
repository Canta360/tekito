// tekito_ja_probe: the candidate list as the text service shows it, for
// checking by eye what people get for everyday input (emoji, symbols, dates,
// katakana words...). Each line of the input is keys typed in Japanese
// mode; for each, the first pages of the list after Space are printed.
//
//   TEKITO_DATA_PACK_DIR=<data> tekito_ja_probe <keys file> [rows=27] [live] [mix]
//
// "live" turns live conversion on (the text after a pause is printed too);
// "mix" types in Mix, "mixlive" with its live conversion; "steps" prints
// what is shown after each key; "nocheck" turns the sentence check off.
//
// The composer is set up as the text service sets it up: the installed
// Japanese data and the default settings.
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/ConversionEngine.h"
#include "Core/EnglishSpelling.h"
#include "Core/ExternalLexiconProvider.h"
#include "UserData/JapaneseSettings.h"
#include "UserData/UserSettings.h"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tekito_ja_probe <keys file> [rows]\n";
        return 2;
    }
    // "score" as the second argument: each line is a sentence, printed with
    // what the sentence check (japanese-reranker) makes of it.
    if (argc > 2 && std::string(argv[2]) == "score") {
        tekito::japanese::SentenceReranker reranker;
        if (!reranker.Open(tekito::ExternalLexiconProvider::PackDirectory(L"japanese-reranker"))) return 3;
        std::ifstream lines(argv[1], std::ios::binary);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const int size = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
            std::wstring sentence(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), sentence.data(), size);
            const std::wstring one[] = {sentence};
            std::cout << line << '\t' << reranker.LogProbabilities(one).front() << '\n';
        }
        return 0;
    }
    // "topic" as the second argument: each line is two words and a tab
    // between, printed with how often each is seen (ln count) and how much
    // they share sentences (LanguageModel::Topic), as the language model
    // (japanese-lm) has them.
    if (argc > 2 && std::string(argv[2]) == "topic") {
        tekito::japanese::LanguageModel model;
        if (!model.Open(tekito::ExternalLexiconProvider::PackDirectory(L"japanese-lm"))) return 3;
        std::ifstream lines(argv[1], std::ios::binary);
        const auto show = [](const std::optional<double>& value) {
            return value ? std::to_string(*value).substr(0, 5) : std::string("-");
        };
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            const std::wstring a = Widen(line.substr(0, tab)), b = Widen(line.substr(tab + 1));
            std::cout << line << "\tcount " << show(model.LogCount(a)) << " " << show(model.LogCount(b)) << "\ttopic "
                      << show(model.Topic(a, b)) << "\tpair " << show(model.PairPmi(tekito::japanese::LanguageModel::PairStart(a), b))
                      << "\n";
        }
        return 0;
    }
    // "spell" as the second argument: each line is a word and, after a tab,
    // the English before it, printed with what Mix's English correction
    // (English Auto's Space) makes of it.
    if (argc > 2 && std::string(argv[2]) == "spell") {
        tekito::PreloadDefaultEngineData();
        const auto engine = tekito::CreateDefaultConversionEngine();
        if (!engine) return 3;
        const tekito::EnglishSpelling spelling(*engine);
        std::ifstream lines(argv[1], std::ios::binary);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto tab = line.find('\t');
            const auto result = spelling.Spell(Widen(line.substr(0, tab)), tab == std::string::npos ? L"" : Widen(line.substr(tab + 1)));
            std::string others;
            for (const auto& other : result.others) others += " " + Narrow(other);
            std::cout << line << "\t" << Narrow(result.word) << (result.corrected ? "\tcorrected" : "\tkept") << "\t" << others << "\n";
        }
        return 0;
    }
    const std::size_t rows = argc > 2 && std::isdigit(static_cast<unsigned char>(argv[2][0])) ? std::stoul(argv[2]) : 27;
    tekito::japanese::JapaneseComposer composer(tekito::japanese::ProcessRomajiTable());
    tekito::japanese::AttachJapaneseData(composer);
    // "lattice" as the second argument: each line is a reading in kana,
    // printed with the converter's likeliest path, each word's cost and the
    // join into it (connection, pairs, compounds), and beside each the other
    // words over the same span with their joins from the word before.
    if (argc > 2 && std::string(argv[2]) == "lattice") {
        const auto* converter = composer.Converter();
        if (!converter) return 3;
        std::ifstream lines(argv[1], std::ios::binary);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto lattice = converter->ExportLattice(Widen(line));
            std::cout << line << "\tbest " << lattice.bestCost << "\n";
            const auto join = [&](int from, int to) -> std::optional<std::int64_t> {
                for (const auto& j : lattice.joins) {
                    if (j.left == from && j.right == to) return j.cost;
                }
                return std::nullopt;
            };
            int previous = -1;
            for (std::size_t b = 0; b < lattice.best.size(); ++b) {
                const int k = lattice.best[b];
                const auto& word = lattice.words[static_cast<std::size_t>(k)];
                const int next = b + 1 < lattice.best.size() ? lattice.best[b + 1] : -1;
                const auto show = [&](int index) {
                    const auto& w = lattice.words[static_cast<std::size_t>(index)];
                    const auto in = previous >= 0 ? join(previous, index) : std::nullopt;
                    const auto out = next >= 0 ? join(index, next) : std::nullopt;
                    return Narrow(w.text) + "(" + std::to_string(w.cost) + " in " + (in ? std::to_string(*in) : "-") + " out " +
                           (out ? std::to_string(*out) : "-") + ")";
                };
                std::cout << "  " << show(k) << "   vs";
                int shown = 0;
                for (std::size_t o = 0; o < lattice.words.size() && shown < 6; ++o) {
                    const auto& other = lattice.words[o];
                    if (static_cast<int>(o) == k || other.begin != word.begin || other.end != word.end) continue;
                    std::cout << " " << show(static_cast<int>(o));
                    ++shown;
                }
                std::cout << "\n";
                previous = k;
            }
        }
        return 0;
    }
    tekito::userdata::UserSettings settings;
    bool mix = false;
    bool steps = false;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "mixlive") settings.mixLiveConversion = true;
        if (std::string(argv[i]) == "steps") steps = true;
        if (std::string(argv[i]) == "live") settings.japaneseLiveConversion = true;
        if (std::string(argv[i]) == "mix") mix = true;
        if (std::string(argv[i]) == "nocheck") settings.japaneseSentenceCheck = false;
        if (std::string(argv[i]).starts_with("num=")) settings.japaneseNumberStyle = std::stoi(argv[i] + 4);
        if (std::string(argv[i]).starts_with("kanji=")) settings.japaneseKanjiRange = std::stoi(argv[i] + 6);
        if (std::string(argv[i]).starts_with("grade=")) settings.japaneseKanjiGrade = std::stoi(argv[i] + 6);
    }
    tekito::userdata::ApplyJapaneseSettings(composer, settings, nullptr);
    tekito::japanese::AttachMixedTyping(composer, mix);
    std::ifstream in(argv[1], std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        composer.Clear();
        composer.ForgetContext();
        // What is shown: the segments, as the text service writes them.
        const auto shown = [&] {
            std::wstring text;
            for (const auto& segment : composer.Segments()) text += segment.text;
            return text;
        };
        // A space is the Space key (Mix settles a chunk with it).
        std::wstring shownSteps;
        for (const wchar_t key : Widen(line)) {
            if (key != L' ') {
                if (composer.CommitsBeforeTyping() && !mix) composer.Commit();
                composer.Insert(key);
            } else {
                composer.Convert();
            }
            if (steps) shownSteps += (shownSteps.empty() ? L"" : L" > ") + shown();
        }
        std::cout << line << "\t" << Narrow(shown());
        if (steps) std::cout << "\n  steps: " << Narrow(shownSteps) << "\n ";
        std::string predictions;
        for (const auto& prediction : composer.Predictions()) predictions += " " + Narrow(prediction.text);
        if (!predictions.empty()) std::cout << "\tpredicted:" << predictions;
        if (settings.japaneseLiveConversion && composer.RefineLive()) std::cout << "\tpaused: " << Narrow(composer.Preedit());
        composer.Convert();
        std::cout << "\tspace: " << Narrow(composer.Preedit());
        std::wstring phrases;
        for (const auto& segment : composer.Segments()) phrases += (phrases.empty() ? L"" : L"|") + segment.text;
        std::cout << "\tphrases: " << Narrow(phrases) << "\n  ";
        if (const auto* list = composer.FocusedCandidates()) {
            for (std::size_t i = 0; i < list->size() && i < rows; ++i) {
                std::cout << (i ? " | " : "") << Narrow((*list)[i].text);
            }
            std::cout << "  (" << list->size() << ")";
        }
        std::cout << "\n";
    }
    return 0;
}
