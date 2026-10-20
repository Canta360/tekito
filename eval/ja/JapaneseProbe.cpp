// tekito_ja_probe: the candidate list as the text service shows it, for
// checking by eye what people get for everyday input (emoji, symbols, dates,
// katakana words...). Each line of the input is keys typed in Japanese
// mode; for each, the first pages of the list after Space are printed.
//
//   TEKITO_DATA_PACK_DIR=<data> tekito_ja_probe <keys file> [rows=27] [live] [mix]
//
// "live" turns live conversion on (the text after a pause is printed too);
// "mix" types in Mix, "mixlive" with its live conversion; "steps" prints
// what is shown after each key.
//
// The composer is set up as the text service sets it up: the installed
// Japanese data and the default settings.
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseData.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/ConversionEngine.h"
#include "Core/EnglishSpelling.h"
#include "Core/ExternalLexiconProvider.h"
#include "UserData/JapaneseSettings.h"
#include "UserData/UserSettings.h"

#include <windows.h>

#include <fstream>
#include <iostream>
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
    const std::size_t rows = argc > 2 ? std::stoul(argv[2]) : 27;
    tekito::japanese::JapaneseComposer composer(tekito::japanese::ProcessRomajiTable());
    tekito::japanese::AttachJapaneseData(composer);
    tekito::userdata::UserSettings settings;
    bool mix = false;
    bool steps = false;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "mixlive") settings.mixLiveConversion = true;
        if (std::string(argv[i]) == "steps") steps = true;
        if (std::string(argv[i]) == "live") settings.japaneseLiveConversion = true;
        if (std::string(argv[i]) == "mix") mix = true;
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
