#include "Core/DataPackPath.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseKeys.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/ChunkJudge.h"
#include "Core/RankingData.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/KanjiSets.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/PostalCodes.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/Japanese/SentenceReranker.h"
#include "Core/Japanese/KanaSlips.h"
#include "Core/Japanese/SortedTsv.h"
#include "Core/SpecialConversions.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "Core/ConversionEngine.h"
#include "Core/EnglishSpelling.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/Utf8.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using tekito::japanese::JapaneseComposer;
using tekito::japanese::KanaForm;
using tekito::japanese::PunctuationStyle;
using tekito::japanese::RomajiTable;

std::string Utf8(std::wstring_view text) {
#if defined(_WIN32)
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                                           0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
#else
    return tekito::utf8::Encode(text);
#endif
}

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void RequireText(std::wstring_view actual, std::wstring_view expected, const char* message) {
    if (actual != expected) {
        std::cerr << "FAILED: " << message << "\n  expected: " << Utf8(expected)
                  << "\n  actual:   " << Utf8(actual) << '\n';
        std::exit(1);
    }
}

std::unique_ptr<RomajiTable> LoadTable() {
    auto table = RomajiTable::Load(tekito::ExternalLexiconProvider::PackDirectory(L"japanese-romaji"));
    Require(table != nullptr, "the japanese-romaji pack loads");
    return table;
}

std::wstring Type(JapaneseComposer& composer, std::wstring_view keys) {
    for (wchar_t key : keys) composer.Insert(key);
    return composer.Preedit();
}

std::wstring Committed(const RomajiTable& table, std::wstring_view keys) {
    JapaneseComposer composer(&table);
    Type(composer, keys);
    return composer.Commit();
}

void TestKanaText() {
    using namespace tekito::japanese;
    RequireText(ToKatakana(L"がっこう、ゔぁ"), L"ガッコウ、ヴァ", "hiragana to katakana");
    RequireText(ToHiragana(L"ガッコウヴヵ"), L"がっこうゔゕ", "katakana to hiragana");
    RequireText(ToHalfWidthKatakana(L"がっこう"), L"ｶﾞｯｺｳ", "voiced kana become base plus mark");
    RequireText(ToHalfWidthKatakana(L"パーティー。"), L"ﾊﾟｰﾃｨｰ｡", "semi-voiced kana and punctuation");
    RequireText(ToHalfWidthKatakana(L"ヴＡ１"), L"ｳﾞA1", "vu and full-width ASCII");
    RequireText(ToFullWidthAscii(L"Ab 1!"), L"Ａｂ　１！", "ASCII to full-width");
    RequireText(ToHalfWidthAscii(L"Ａｂ　１！"), L"Ab 1!", "full-width back to ASCII");
    RequireText(ToKatakana(L"漢字abc"), L"漢字abc", "other text is left alone");
}

void TestRomajiChanges() {
    // The user's changes over the pack's rows, kept in a file.
    auto table = LoadTable();
    const std::vector<RomajiTable::Change> changes{{L"la", L"ら", L""}, {L"nn", L"", L""}, {L";", L"っ", L""}};
    table->ApplyChanges(changes);
    Require(table->Find(L"la") && table->Find(L"la")->output == L"ら", "a changed row types its kana");
    Require(!table->Find(L"nn"), "a row taken out is not used");
    Require(table->Find(L";") && table->Find(L";")->output == L"っ", "an added row types its kana");
    Require(table->PackRule(L"la") && table->PackRule(L"la")->output == L"ぁ", "the pack's own row is still known");
    const auto file = std::filesystem::temp_directory_path() / L"tekito-test-romaji.tsv";
    Require(RomajiTable::WriteChanges(file, changes), "changes are written");
    const auto read = RomajiTable::ReadChanges(file);
    Require(read.size() == 3 && read[1].input == L"nn" && read[1].output.empty(), "and read back, a taken-out row too");
    table->ApplyChanges({});
    Require(table->Find(L"nn") && table->Find(L"la")->output == L"ぁ", "no changes: the pack's table again");
    // Another IME's whole table: its differences, and what it leaves out.
    std::vector<RomajiTable::Change> other;
    for (const wchar_t* row : {L"ka", L"ki", L"ku", L"ke", L"ko"}) other.push_back({row, table->Find(row)->output, L""});
    other.push_back({L"la", L"ら", L""});
    const auto few = table->ChangesFor(other);
    Require(few.size() == 1 && few[0].input == L"la", "a few rows: only what differs");
    std::filesystem::remove(file);
}

void TestRomajiTable(const RomajiTable& table) {
    Require(table.Size() > 300, "the romaji table has the usual rows");
    const auto* ka = table.Find(L"ka");
    Require(ka && ka->output == L"か" && ka->pending.empty(), "ka is か");
    const auto* kk = table.Find(L"kk");
    Require(kk && kk->output == L"っ" && kk->pending == L"k", "kk is っ with k pending");
    Require(table.HasLongerInput(L"n") && table.HasLongerInput(L"ky"), "n and ky can continue");
    Require(!table.HasLongerInput(L"ka"), "ka is complete");
    RequireText(table.KeysFor(L"か"), L"ka", "か is typed ka, not ca");
    RequireText(table.KeysFor(L"し"), L"si", "し is typed with the shortest keys");
}

void TestRomajiConversion(const RomajiTable& table) {
    RequireText(Committed(table, L"konnnichiha"), L"こんにちは", "nn then ni");
    RequireText(Committed(table, L"shinbun"), L"しんぶん", "n before a consonant");
    RequireText(Committed(table, L"kanji"), L"かんじ", "n before j");
    RequireText(Committed(table, L"n'a"), L"んあ", "n' ends the n");
    RequireText(Committed(table, L"kitte"), L"きって", "doubled consonant");
    RequireText(Committed(table, L"matcha"), L"まっちゃ", "tch");
    RequireText(Committed(table, L"nyuusu"), L"にゅうす", "nyu");
    RequireText(Committed(table, L"fairu"), L"ふぁいる", "fa");
    RequireText(Committed(table, L"vaiorin"), L"ゔぁいおりん", "va and a final n");
    RequireText(Committed(table, L"desu."), L"です。", "period");
    RequireText(Committed(table, L"ra-men"), L"らーめん", "long vowel mark");
    RequireText(Committed(table, L"[a]"), L"「あ」", "brackets");
    RequireText(Committed(table, L"123"), L"１２３", "digits are full-width");
    RequireText(Committed(table, L"#"), L"＃", "the # row is a key, not a comment");
    RequireText(Committed(table, L"xtu"), L"っ", "small tsu");
    RequireText(Committed(table, L"kyk"), L"ｋｙｋ", "keys that never become kana stay as typed");

    JapaneseComposer composer(&table);
    RequireText(Type(composer, L"k"), L"ｋ", "a pending key shows full-width");
    RequireText(Type(composer, L"y"), L"ｋｙ", "two pending keys");
    RequireText(Type(composer, L"a"), L"きゃ", "kya");
    RequireText(Type(composer, L"n"), L"きゃｎ", "a final n waits");
    RequireText(composer.Commit(), L"きゃん", "commit settles the n");
    Require(!composer.IsComposing(), "commit clears the composition");
}

void TestEditing(const RomajiTable& table) {
    JapaneseComposer composer(&table);
    Type(composer, L"kya");
    composer.Backspace();
    RequireText(composer.Preedit(), L"き", "backspace removes one kana");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(composer.Preedit(), L"ki", "the remaining kana keeps its keys");
    composer.Backspace();
    RequireText(composer.Preedit(), L"き", "backspace after F10 returns to kana");
    composer.Backspace();
    Require(!composer.IsComposing(), "deleting the last kana ends the composition");

    Type(composer, L"sh");
    composer.Backspace();
    RequireText(composer.Preedit(), L"ｓ", "backspace removes a pending key");
    composer.Cancel();
    Require(!composer.IsComposing(), "Esc drops typed text");
}

void TestCaret(const RomajiTable& table) {
    JapaneseComposer composer(&table);
    Type(composer, L"kyouha");
    Require(composer.CaretOffset() == 4, "the caret follows typing");
    composer.MoveCaret(-2);
    Require(composer.CaretOffset() == 2, "Left moves by kana");
    RequireText(Type(composer, L"ka"), L"きょかうは", "typing goes in at the caret");
    Require(composer.CaretOffset() == 3, "the caret is after what was typed");
    composer.Insert(L'k');
    RequireText(composer.Preedit(), L"きょかｋうは", "pending keys show at the caret");
    composer.Backspace();
    composer.Backspace();
    RequireText(composer.Preedit(), L"きょうは", "Backspace removes before the caret");
    composer.MoveCaret(-1);
    RequireText(Type(composer, L"a"), L"きあょうは", "the caret can stop inside きょ");
    composer.MoveCaret(-100);
    Require(composer.CaretOffset() == 0, "Home goes to the start");
    composer.Delete();
    RequireText(composer.Preedit(), L"あょうは", "Delete removes after the caret");
    composer.MoveCaret(100);
    Require(composer.CaretOffset() == 4, "End goes to the end");
    composer.Convert();
    composer.Cancel();
    RequireText(Type(composer, L"ne"), L"あょうはね", "after a conversion, typing goes on at the end");

    composer.Clear();
    RequireText(Type(composer, L"ktoba"), L"ｋとば", "a consonant without its vowel stays as typed");
    composer.MoveCaret(-2);
    RequireText(Type(composer, L"o"), L"ことば", "a vowel typed right after it later joins it");
    composer.Clear();
    RequireText(Type(composer, L"zhzjzkzl"), L"←↓↑→", "z and h, j, k, l type arrows");
    RequireText(Type(composer, L"z.z/"), L"←↓↑→…・", "and z with marks, marks");
    composer.Clear();
    // かな入力: kana come as kana, and the sound marks join the kana before them.
    RequireText(Type(composer, L"か゛き"), L"がき", "か then ゛ is が");
    RequireText(Type(composer, L"は゜"), L"がきぱ", "は then ゜ is ぱ");
    RequireText(Type(composer, L"う゛"), L"がきぱゔ", "う then ゛ is ゔ");
    RequireText(Type(composer, L"あ゛"), L"がきぱゔあ゛", "a mark that joins nothing stays");
    composer.Clear();
}

void TestConversionForms(const RomajiTable& table) {
    JapaneseComposer composer(&table);
    {
        // Up from the first candidate goes round to the forms of what was
        // typed: katakana, half-width katakana, the keys in both widths and
        // cases, small kana.
        JapaneseComposer one(&table);
        Type(one, L"a");
        one.Convert();
        std::wstring reached;
        for (int i = 0; i < 7; ++i) {
            one.PreviousCandidate();
            reached += one.Preedit() + L" ";
        }
        RequireText(reached, L"あ ｱ ａ Ａ a A ぁ ", "Up reaches the forms of a typed alone (ア is first)");
    }
    {
        // How a number converts first.
        using Style = JapaneseComposer::NumberStyle;
        JapaneseComposer number(&table);
        number.SetHalfWidthDigits(true);
        const auto converted = [&](std::wstring_view keys, Style style) {
            number.Clear();
            number.SetNumberStyle(style);
            Type(number, keys);
            number.Convert();
            return number.Preedit();
        };
        RequireText(converted(L"15", Style::AsTyped), L"15", "as typed: in the width digits are typed in");
        RequireText(converted(L"15", Style::FullWidth), L"１５", "full-width");
        RequireText(converted(L"3", Style::Mixed), L"３", "mixed: one digit full-width");
        RequireText(converted(L"15", Style::Mixed), L"15", "and more half-width");
        RequireText(converted(L"006", Style::Official), L"006", "digits from 0 are a code: none dropped");
        RequireText(converted(L"0120", Style::Kanji), L"0120", "nor written in kanji");
        number.SetHalfWidthDigits(false);
        RequireText(converted(L"15", Style::HalfWidth), L"15", "half-width, even typed full-width");
        // Typed with one digit full-width: as it shows before conversion.
        number.SetHalfWidthDigits(true);
        number.SetSingleDigitFullWidth(true);
        const auto typed = [&](std::wstring_view keys) {
            number.Clear();
            Type(number, keys);
            return number.Preedit();
        };
        RequireText(typed(L"3"), L"３", "one digit typed is full-width");
        RequireText(typed(L"15"), L"15", "two are half-width");
        RequireText(typed(L"1.5"), L"1.5", "a decimal is one number");
        RequireText(typed(L"3ko"), L"３こ", "a digit before kana too");
        RequireText(converted(L"3", Style::AsTyped), L"３", "and it converts as typed");
        number.SetSingleDigitFullWidth(false);
    }
    Type(composer, L"katakana");
    composer.Convert();
    Require(composer.IsConverted(), "Space converts");
    RequireText(composer.Preedit(), L"カタカナ", "Space gives katakana before the dictionary");
    composer.PreviousCandidate();
    RequireText(composer.Preedit(), L"かたかな", "Up goes round to hiragana");
    composer.Cancel();
    Require(!composer.IsConverted() && composer.IsComposing(), "Esc returns to the typed kana");
    RequireText(composer.Preedit(), L"かたかな", "the typed kana is kept");
    composer.Convert();
    {
        const auto* list = composer.FocusedCandidates();
        const auto has = [&](std::wstring_view text) {
            return list && std::any_of(list->begin(), list->end(), [&](const auto& c) { return c.text == text; });
        };
        Require(has(L"ｶﾀｶﾅ") && has(L"ｋａｔａｋａｎａ") && has(L"katakana"),
                "the list ends with half-width katakana and the keys in both widths");
        RequireText(list ? list->front().text : L"", L"カタカナ", "they come after the rest");
    }
    composer.Cancel();
    composer.SetTypedForms(false);
    composer.Convert();
    {
        const auto* list = composer.FocusedCandidates();
        Require(list && std::none_of(list->begin(), list->end(), [](const auto& c) { return c.text == L"katakana"; }),
                "with the typed forms off, the list does not end with them");
    }
    composer.Cancel();
    composer.SetTypedForms(true);

    composer.CycleKana();
    RequireText(composer.Preedit(), L"カタカナ", "Muhenkan: katakana");
    composer.CycleKana();
    RequireText(composer.Preedit(), L"ｶﾀｶﾅ", "Muhenkan: half-width katakana");
    composer.CycleKana();
    RequireText(composer.Preedit(), L"かたかな", "Muhenkan: back to hiragana");

    composer.Clear();
    Type(composer, L"Tekito");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(composer.Preedit(), L"Tekito", "F10: as typed");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(composer.Preedit(), L"TEKITO", "F10 again: upper case");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(composer.Preedit(), L"Tekito", "F10 again: capitalized");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(composer.Preedit(), L"tekito", "F10 again: lower case");
    composer.Transliterate(KanaForm::FullWidthAlphanumeric);
    RequireText(composer.Preedit(), L"Ｔｅｋｉｔｏ", "F9: full-width, as typed");
    composer.Transliterate(KanaForm::Katakana);
    RequireText(composer.Commit(), L"テキト", "F7: katakana, read from the keys");

    composer.SetFieldForm(KanaForm::Katakana);
    RequireText(Type(composer, L"furigana"), L"フリガナ", "a katakana field types katakana");
    composer.Clear();
    composer.SetFieldForm(std::nullopt);
    RequireText(Type(composer, L"furigana"), L"ふりがな", "other fields follow the input mode");
    composer.Clear();
}

void TestInputFormAndPunctuation(const RomajiTable& table) {
    JapaneseComposer composer(&table);
    composer.SetInputForm(KanaForm::Katakana);
    RequireText(Type(composer, L"tesuto"), L"テスト", "katakana input");
    composer.Convert();
    RequireText(composer.Preedit(), L"てすと", "Space from katakana input gives hiragana");
    composer.Clear();

    composer.SetInputForm(KanaForm::Hiragana);
    composer.SetPunctuationStyle(PunctuationStyle::CommaPeriod);
    RequireText(Committed(table, L"a,i."), L"あ、い。", "default marks");
    Type(composer, L"a,i.");
    RequireText(composer.Commit(), L"あ，い．", "comma and period");
    composer.SetPunctuationStyle(PunctuationStyle::CommaKuten);
    Type(composer, L"a,i.");
    RequireText(composer.Commit(), L"あ，い。", "comma and kuten");
}

struct MiniPack {
    tekito::japanese::JapaneseDictionary dictionary;
    tekito::japanese::ConnectionMatrix matrix;
};

std::unique_ptr<MiniPack> LoadMiniPack() {
    auto pack = std::make_unique<MiniPack>();
    const std::filesystem::path root = tekito::FindDataPack(TEKITO_TEST_DATA_DIR, L"japanese-mini");
    Require(pack->dictionary.Open(root / L"dictionary.bin"), "the test dictionary opens");
    Require(pack->matrix.Open(root / L"connection.bin"), "the test connection matrix opens");
    return pack;
}

std::wstring Joined(const std::vector<tekito::japanese::Phrase>& phrases, bool markPhrases) {
    std::wstring out;
    for (const auto& phrase : phrases) {
        if (markPhrases && !out.empty()) out += L'|';
        if (!phrase.candidates.empty()) out += phrase.candidates.front().text;
    }
    return out;
}

void DumpConversions(const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    for (const wchar_t* reading : {L"わたしのなまえはなかのです", L"きょうはいいてんきですね",
                                   L"にほんごをにゅうりょくする", L"かんじにへんかんします",
                                   L"あしたはあめがふるでしょう", L"とうきょうにすんでいます",
                                   L"ここではきものをぬぐ", L"きかいがとまる"}) {
        const auto phrases = converter.Convert(reading);
        std::cout << Utf8(reading) << " -> " << Utf8(Joined(phrases, true)) << '\n';
        for (const auto& phrase : phrases) {
            std::cout << "   ";
            for (std::size_t i = 0; i < phrase.candidates.size() && i < 8; ++i) {
                std::cout << ' ' << Utf8(phrase.candidates[i].text);
            }
            std::cout << '\n';
        }
    }
}

bool HasCandidate(const tekito::japanese::Phrase& phrase, std::wstring_view text, std::size_t within) {
    for (std::size_t i = 0; i < phrase.candidates.size() && i < within; ++i) {
        if (phrase.candidates[i].text == text) return true;
    }
    return false;
}

// Words added from a pack of their own (japanese-wikimedia), looked up with
// the dictionary's as one.
void TestAddedWords(const MiniPack& pack) {
    using tekito::japanese::DictionaryWord;
    const std::filesystem::path data(TEKITO_TEST_DATA_DIR);
    tekito::japanese::JapaneseDictionary dictionary;
    Require(dictionary.Open(data / L"japanese-mini" / L"dictionary.bin"), "the test dictionary opens again");
    Require(!dictionary.AddWords(data / L"japanese-mini" / L"connection.bin") && !dictionary.HasAddedWords(),
            "a file that is not a dictionary adds nothing");
    Require(dictionary.AddWords(data / L"japanese-mini-wikimedia" / L"dictionary.bin"), "the added words open");

    // A reading only the added words have.
    const auto added = dictionary.Find(dictionary.Encode(L"てんきなかの"));
    Require(added.has_value() && *added >= pack.dictionary.KeyCount(), "a reading of the added words is found after the dictionary's");
    std::wstring surfaces;
    dictionary.ForEachWord(*added, [&](const DictionaryWord& word) { surfaces += dictionary.Surface(word, L"てんきなかの"); });
    RequireText(surfaces, L"天気中野", "its word is written as added");
    RequireText(dictionary.KeyText(*added), L"てんきなかの", "and its key reads so");

    // A reading both have: one record, the words cheapest first.
    const auto shared = dictionary.Find(dictionary.Encode(L"きかい"));
    Require(shared.has_value() && *shared < pack.dictionary.KeyCount(), "a reading both have is the dictionary's record");
    bool machine = false, addedWord = false, sorted = true;
    int previousCost = -1;
    dictionary.ForEachWord(*shared, [&](const DictionaryWord& word) {
        const auto text = dictionary.Surface(word, L"きかい");
        machine = machine || text == L"機械";
        addedWord = addedWord || (text == L"奇械" && word.added);
        sorted = sorted && static_cast<int>(word.cost) >= previousCost;
        previousCost = word.cost;
    });
    Require(machine && addedWord && sorted, "it has the dictionary's words and the added one, cheapest first");
    std::size_t stored = 0;
    dictionary.ForEachStoredWord(*shared, [&](const DictionaryWord& word) { stored += word.added ? 100 : 1; });
    Require(stored > 0 && stored < 100, "the record holds only the dictionary's own words");

    // Prefixes from both, shortest first, a shared key once.
    std::vector<std::size_t> lengths;
    dictionary.CommonPrefixSearch(dictionary.Encode(L"てんきなかのです"),
                                  [&](std::size_t length, std::uint32_t) { lengths.push_back(length); });
    Require(std::is_sorted(lengths.begin(), lengths.end()) &&
                std::adjacent_find(lengths.begin(), lengths.end()) == lengths.end() &&
                std::find(lengths.begin(), lengths.end(), 6) != lengths.end(),
            "prefixes come shortest first, each once, the added word among them");
    const auto ranges = dictionary.PrefixRanges(dictionary.Encode(L"てんき"));
    Require(ranges[1].second > ranges[1].first, "the added words have keys from てんき on");

    tekito::japanese::JapaneseConverter converter(dictionary, pack.matrix);
    const auto predictions = converter.Predict(L"てんきな", 10);
    Require(std::any_of(predictions.begin(), predictions.end(), [](const auto& p) { return p.text == L"天気中野"; }),
            "an added word is predicted");
}

void TestDictionary(const MiniPack& pack) {
    const auto& dictionary = pack.dictionary;
    const auto record = dictionary.Find(dictionary.Encode(L"きかい"));
    Require(record.has_value(), "the dictionary has きかい");
    bool machine = false, chance = false;
    int previousCost = -1;
    bool sorted = true;
    dictionary.ForEachWord(*record, [&](const tekito::japanese::DictionaryWord& word) {
        const auto text = dictionary.Surface(word, L"きかい");
        machine = machine || text == L"機械";
        chance = chance || text == L"機会";
        sorted = sorted && static_cast<int>(word.cost) >= previousCost;
        previousCost = word.cost;
    });
    Require(machine && chance, "きかい has both 機械 and 機会");
    Require(sorted, "words come cheapest first");
    Require(!dictionary.Find(dictionary.Encode(L"きかいがとまるよ")), "an unknown reading is not found");

    std::size_t prefixes = 0;
    dictionary.CommonPrefixSearch(dictionary.Encode(L"きかいが"), [&](std::size_t length, std::uint32_t) {
        Require(length >= 1 && length <= 4, "prefix lengths stay within the reading");
        ++prefixes;
    });
    Require(prefixes >= 2, "き and きかい are prefixes of きかいが");

    Require(pack.matrix.IsBoundary(0, 1) && pack.matrix.Cost(0, 0) >= 0,
            "the start of the text is a phrase boundary");
}

void TestDamagedPacks() {
    const auto source = std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-mini" / L"dictionary.bin";
    const auto damaged = std::filesystem::temp_directory_path() / L"tekito-damaged-dictionary.bin";
    std::error_code error;
    std::filesystem::copy_file(source, damaged, std::filesystem::copy_options::overwrite_existing, error);
    Require(!error, "the test dictionary copies");
    std::filesystem::resize_file(damaged, std::filesystem::file_size(damaged) / 2, error);
    tekito::japanese::JapaneseDictionary dictionary;
    Require(!dictionary.Open(damaged), "a truncated dictionary does not open");
    {
        std::ofstream garbage(damaged, std::ios::binary | std::ios::trunc);
        garbage << std::string(4096, 'x');
    }
    Require(!dictionary.Open(damaged), "a file that is not a dictionary does not open");
    tekito::japanese::ConnectionMatrix matrix;
    Require(!matrix.Open(damaged), "a file that is not a matrix does not open");
    Require(!dictionary.Open(std::filesystem::temp_directory_path() / L"tekito-missing.bin"),
            "a missing dictionary does not open");
    std::filesystem::remove(damaged, error);
}

void TestConversion(const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    RequireText(Joined(converter.Convert(L"わたしのなまえはなかのです"), true), L"私の|名前は|中野です",
                "a sentence splits into phrases and converts");
    RequireText(Joined(converter.Convert(L"ここではきものをぬぐ"), true), L"ここでは|着物を|脱ぐ",
                "the likelier split wins");

    // A word the dictionary lacks, shaped like a borrowed one: katakana.
    const auto borrowed = converter.Convert(L"ぐろっくです");
    Require(!borrowed.empty() && !borrowed[0].candidates.empty() &&
                borrowed[0].candidates.front().text.starts_with(L"グロック"),
            "kana ending like a borrowed word (っく) convert to one katakana word");

    const auto kanji = converter.Convert(L"かんじにへんかんします");
    Require(kanji.size() == 2 && HasCandidate(kanji[0], L"漢字に", 3),
            "漢字に is among the first candidates of かんじに");
    const auto machine = converter.Convert(L"きかいがとまる");
    Require(HasCandidate(machine[0], L"機械が", 3) && HasCandidate(machine[0], L"機会が", 3),
            "homophones are offered for the phrase");
    for (const auto& phrase : machine) {
        const auto reading = std::wstring_view(L"きかいがとまる").substr(phrase.begin, phrase.length);
        Require(HasCandidate(phrase, reading, 100) &&
                    HasCandidate(phrase, tekito::japanese::ToKatakana(reading), 100),
                "every phrase can stay in hiragana or katakana");
    }

    const std::size_t fixed[] = {5};
    const auto resized = converter.Convert(L"きかいがとまる", fixed);
    Require(!resized.empty() && resized[0].begin == 0 && resized[0].length == 5,
            "a fixed phrase length is kept");
    Require(resized.size() == 2 && resized[1].begin == 5 && resized[1].length == 2,
            "the rest is converted after the fixed phrase");

    RequireText(Joined(converter.Convert(L"ＴＥＫＩＴＯです"), false).substr(0, 6), L"ＴＥＫＩＴＯ",
                "letters the dictionary does not know stay as typed");
    Require(converter.Convert(L"").empty(), "nothing to convert gives no phrases");
}

std::wstring SegmentsText(const JapaneseComposer& composer) {
    std::wstring out;
    for (const auto& segment : composer.Segments()) {
        if (!out.empty()) out += L'|';
        if (segment.focused) out += L'*';
        out += segment.text;
    }
    return out;
}

void TestComposerConversion(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);

    Type(composer, L"watashinonamaehanakanodesu");
    composer.Convert();
    RequireText(SegmentsText(composer), L"*私の|名前は|中野です", "Space converts into phrases");
    Require(!composer.IsCandidateListOpen(), "the first Space does not open the list");
    composer.Convert();
    Require(composer.IsCandidateListOpen(), "the second Space opens the list");
    Require(composer.FocusedSelection() == 1, "the second Space picks the next candidate");
    composer.PreviousCandidate();
    RequireText(SegmentsText(composer), L"*私の|名前は|中野です", "Shift+Space goes back");
    composer.SelectCandidate(0);
    Require(!composer.IsCandidateListOpen(), "choosing a candidate closes the list");

    composer.MoveFocus(1);
    RequireText(SegmentsText(composer), L"私の|*名前は|中野です", "Right moves to the next phrase");
    composer.Transliterate(KanaForm::Katakana);
    RequireText(SegmentsText(composer), L"私の|*ナマエハ|中野です", "F7 writes the focused phrase in katakana");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(SegmentsText(composer), L"私の|*namaeha|中野です", "F10 gives the keys typed for the phrase");
    composer.MoveFocus(5);
    RequireText(SegmentsText(composer), L"私の|namaeha|*中野です", "focus stops at the last phrase");

    composer.MoveFocus(-5);
    composer.ResizeFocus(-1);
    const auto resized = SegmentsText(composer);
    Require(resized.starts_with(L"*私|の"), "Shift+Left shortens the focused phrase to わたし");
    composer.ResizeFocus(-10);
    RequireText(SegmentsText(composer), resized, "a phrase cannot shrink below one character");
    composer.ResizeFocus(1);
    Require(SegmentsText(composer).starts_with(L"*私の|"), "Shift+Right gives the character back");

    composer.Cancel();
    Require(composer.IsComposing() && !composer.IsConverted(), "Esc goes back to the kana");
    composer.Convert();
    RequireText(composer.Commit(), L"私の名前は中野です", "Enter commits every phrase");
    Require(!composer.IsComposing(), "committing clears the composition");

    Type(composer, L"kikaigatomaru");
    composer.Convert();
    const auto* candidates = composer.FocusedCandidates();
    Require(candidates && candidates->size() >= 3, "the focused phrase has candidates");
    composer.Insert(L'a');
    Require(!composer.IsConverted(), "typing after a conversion starts typing again");
}

// What each key does in Japanese mode (JapaneseKeys.h), as the text service
// and the testbed carry it out.
void TestKeys(const RomajiTable& table, const MiniPack& pack) {
    using tekito::japanese::KeyCommand;
    using tekito::japanese::KeyPress;
    using Action = KeyCommand::Action;
    using Key = KeyPress::Key;
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    const tekito::japanese::KeyOptions options;
    const auto press = [](Key key, bool shift = false) {
        KeyPress p;
        p.key = key;
        p.shift = shift;
        return p;
    };
    const auto type = [](wchar_t character) {
        KeyPress p;
        p.key = Key::Character;
        p.character = character;
        if (character >= L'1' && character <= L'9') p.digit = character - L'0';
        return p;
    };
    // Carries out what `key` does; whether it did anything.
    const auto hit = [&](const KeyPress& key, tekito::japanese::KeyOutcome* outcome = nullptr) {
        const auto command = tekito::japanese::TranslateKey(composer, key, options);
        if (!command) return false;
        const auto result = tekito::japanese::ApplyKey(composer, *command);
        if (outcome) *outcome = result;
        return true;
    };
    const auto action = [&](const KeyPress& key) {
        const auto command = tekito::japanese::TranslateKey(composer, key, options);
        return command ? std::optional<Action>(command->action) : std::nullopt;
    };

    Require(!action(press(Key::Tab)), "Tab with nothing typed is the application's");
    Type(composer, L"kana");
    Require(action(press(Key::Tab)) == Action::ShowCandidates, "Tab while typing shows the candidates");
    hit(press(Key::Tab));
    Require(composer.IsConverted() && composer.IsCandidateListOpen(), "converted, with the list open");
    Require(action(press(Key::Tab)) == Action::NextCandidate, "Tab then steps through them");
    Require(action(press(Key::Tab, true)) == Action::PreviousCandidate, "Shift+Tab goes back");
    composer.Clear();
    Require(!action(press(Key::Enter)), "Enter with nothing typed is the application's");
    Require(!action(press(Key::Backspace)), "so is Backspace");
    Require(action(press(Key::Space)) == Action::InsertOutside, "Space with nothing typed types a full-width space");
    Require(!action(press(Key::Space, true)), "Shift+Space types the application's half-width one");
    tekito::japanese::KeyOutcome outcome;
    Require(hit(press(Key::Space), &outcome) && outcome.outside == L"\x3000", "the space is full-width");

    for (const wchar_t key : std::wstring_view(L"watashinonamaehanakanodesu")) Require(hit(type(key)), "letters type");
    RequireText(composer.Preedit(), L"わたしのなまえはなかのです", "letters become kana");
    Require(action(press(Key::Left)) == Action::MoveCaret, "Left moves the caret before conversion");
    KeyPress shortcut = type(L'c');
    shortcut.command = true;
    const auto commitAndPass = tekito::japanese::TranslateKey(composer, shortcut, options);
    Require(commitAndPass && commitAndPass->action == Action::Commit && commitAndPass->letKeyThrough,
            "a shortcut commits the text and goes on to the application");

    Require(hit(press(Key::Space)), "Space converts");
    RequireText(SegmentsText(composer), L"*私の|名前は|中野です", "into phrases");
    Require(tekito::japanese::CandidateListFor(composer, 9).kind == tekito::japanese::CandidateList::Kind::None,
            "the first Space shows no list");
    Require(action(press(Key::Left)) == Action::MoveFocus, "Left moves between phrases once converted");
    Require(action(press(Key::Left, true)) == Action::Resize, "Shift+Left resizes the phrase");
    Require(action(press(Key::Space, true)) == Action::PreviousCandidate, "Shift+Space goes back");
    Require(hit(press(Key::Space)), "Space again");
    const auto list = tekito::japanese::CandidateListFor(composer, 9);
    Require(list.kind == tekito::japanese::CandidateList::Kind::Candidates && list.selected == 1u &&
                list.pageStart == 0 && list.count == std::min<std::size_t>(9, list.rows.size()) &&
                list.rows.front().number == 1,
            "the second Space opens the list on the next candidate");
    Require(hit(type(L'1')), "1 chooses from the list");
    Require(!composer.IsCandidateListOpen() && composer.FocusedSelection() == 0, "the first row");
    Require(hit(type(L'k'), &outcome) && outcome.committed == L"私の名前は中野です",
            "typing on commits the conversion");
    RequireText(composer.Preedit(), L"ｋ", "and starts anew");
    Require(hit(press(Key::Escape)) && !composer.IsComposing(), "Esc drops what was typed");

    for (const wchar_t key : std::wstring_view(L"nakano")) hit(type(key));
    Require(action(press(Key::Up)) == Action::Commit, "Up before conversion commits");
    Require(tekito::japanese::TranslateKey(composer, press(Key::Up), options)->letKeyThrough,
            "and moves on in the application");
    Require(hit(press(Key::Enter), &outcome) && outcome.committed == L"なかの" && !composer.IsComposing(),
            "Enter commits the kana");

    composer.SetPredictionEnabled(true);
    for (const wchar_t key : std::wstring_view(L"ariga")) hit(type(key));
    Require(action(press(Key::Tab)) == Action::NextPrediction, "Tab steps into the predictions");
    hit(press(Key::Tab));
    const auto predictions = tekito::japanese::CandidateListFor(composer, 9);
    Require(predictions.kind == tekito::japanese::CandidateList::Kind::Predictions && predictions.selected == 0u,
            "the list shows the predictions with the first chosen");
    Require(action(press(Key::Escape)) == Action::ClearPrediction, "Esc steps out of the predictions first");
    KeyCommand click;
    click.action = Action::ChooseRow;
    click.index = 0;
    outcome = tekito::japanese::ApplyKey(composer, click);
    Require(outcome.committed == predictions.rows.front().text && !composer.IsComposing(),
            "a click on a prediction commits it");
}

// Live conversion (Settings): the text is shown converted while typing.
void TestLiveConversion(const RomajiTable& table, const MiniPack& pack) {
    using tekito::japanese::KeyCommand;
    using tekito::japanese::KeyPress;
    using Action = KeyCommand::Action;
    using Key = KeyPress::Key;
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetLiveConversion(true);
    const tekito::japanese::KeyOptions options;
    const auto press = [](Key key) {
        KeyPress p;
        p.key = key;
        return p;
    };
    const auto action = [&](const KeyPress& key) {
        const auto command = tekito::japanese::TranslateKey(composer, key, options);
        return command ? std::optional<Action>(command->action) : std::nullopt;
    };

    RequireText(Type(composer, L"watashinonamaehanakanodesu"), L"私の名前は中野です", "typing shows the conversion");
    Require(composer.IsLivePreview() && composer.IsTyping() && !composer.CommitsBeforeTyping(),
            "it is still being typed");
    RequireText(SegmentsText(composer), L"私の|名前は|中野です", "no phrase is focused while typing");
    RequireText(Type(composer, L"k"), L"私の名前は中野ですｋ", "a pending key shows as typed");
    composer.Backspace();
    RequireText(composer.Preedit(), L"私の名前は中野です", "Backspace drops the pending key");
    composer.Backspace();
    RequireText(composer.Preedit().substr(0, 5), L"私の名前は", "Backspace drops a kana and converts again");
    Type(composer, L"su");
    RequireText(composer.Preedit(), L"私の名前は中野です", "and typing it back gives the same");

    Require(action(press(Key::Left)) == Action::MoveCaret, "Left moves the caret in the typed text");
    composer.MoveCaret(-2);
    RequireText(composer.Preedit(), L"わたしのなまえはなかのです", "the kana show while the caret is inside");
    Require(composer.CaretOffset() == 11, "with the caret where it moved");
    RequireText(Type(composer, L"ra"), L"わたしのなまえはなかのらです", "typing goes in at the caret");
    composer.Backspace();
    composer.MoveCaret(100);
    RequireText(composer.Preedit(), L"私の名前は中野です", "the conversion shows again at the end");
    composer.MoveFocus(-1);
    RequireText(SegmentsText(composer), L"私の|名前は|*中野です", "picking starts from the last phrase");
    Require(action(press(Key::Left)) == Action::MoveFocus, "then Left steps through the phrases");
    composer.MoveFocus(-1);
    RequireText(SegmentsText(composer), L"私の|*名前は|中野です", "and moving on");
    composer.Cancel();
    RequireText(composer.Preedit(), L"わたしのなまえはなかのです", "Esc shows the kana");
    Require(composer.IsComposing() && !composer.IsConverted(), "and keeps the text");
    composer.Cancel();
    Require(!composer.IsComposing(), "a second Esc drops it");

    Type(composer, L"kikaigatomaru");
    const std::wstring shown = composer.Preedit();
    composer.Convert();
    Require(!composer.IsLivePreview() && composer.IsConverted(), "Space starts picking");
    const auto* candidates = composer.FocusedCandidates();
    Require(candidates && candidates->size() >= 2, "the last phrase has candidates");
    Require(composer.FocusedSelection() == 1, "Space picks its next candidate");
    Require(!composer.IsCandidateListOpen(), "without the list");
    const std::wstring picked = composer.Preedit();
    Require(picked != shown, "the shown text changes");
    composer.Convert();
    Require(composer.IsCandidateListOpen(), "the second Space opens the list");
    composer.PreviousCandidate();
    composer.SelectCandidate(composer.FocusedSelection());
    RequireText(composer.Preedit(), picked, "back to the picked one");
    Require(action(press(Key::Escape)) == Action::Cancel, "Esc while picking");

    KeyPress letter;
    letter.key = Key::Character;
    letter.character = L'n';
    const auto typed = tekito::japanese::ApplyKey(composer, *tekito::japanese::TranslateKey(composer, letter, options));
    Require(!typed.committed, "typing after Space does not commit");
    Type(composer, L"o");
    Require(composer.Preedit().starts_with(picked), "and keeps what was picked");
    Require(composer.IsLivePreview(), "the rest is shown converted");
    const std::wstring before = composer.Preedit();
    RequireText(composer.Commit(), before, "Enter commits what is shown");
    Require(!composer.IsComposing(), "and clears it");

    // Left with a key still pending: it becomes kana first, and the focus
    // lands on a phrase that exists.
    Type(composer, L"nakanon");
    composer.MoveFocus(-1);
    Require(composer.IsConverted() && !composer.IsLivePreview(), "Left with a pending key starts picking");
    const auto segments = composer.Segments();
    Require(std::count_if(segments.begin(), segments.end(), [](const auto& s) { return s.focused; }) == 1,
            "one phrase is focused");
    composer.Clear();
}

// Shift+letter: letters as typed, in half-width, until committed.
void TestCapitals(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    RequireText(Type(composer, L"Google"), L"Google", "letters after a capital stay letters");
    composer.Convert();
    const auto* candidates = composer.FocusedCandidates();
    Require(candidates && candidates->size() >= 3, "Space offers the other forms");
    RequireText(composer.Preedit(), L"Google", "as typed first");
    RequireText(composer.Commit(), L"Google", "and commits as typed");
    RequireText(Type(composer, L"ka"), L"か", "committing goes back to kana");
    composer.Clear();
    RequireText(Type(composer, L"NHKno"), L"NHKの", "a small letter after two capitals goes back to kana");
    composer.Convert();
    Require(composer.Segments().front().text == L"NHK", "the letters are one phrase");
    composer.Clear();
    RequireText(Type(composer, L"Windows11"), L"Windows11", "digits after a capital stay half-width");
    composer.Clear();
    RequireText(Type(composer, L"kyouhaMac"), L"きょうはMac", "a capital in the middle starts letters there");
    composer.Clear();
    RequireText(Type(composer, L"kA"), L"ｋA", "a pending key settles before the capital");
    composer.Clear();
    composer.SetLiveConversion(true);
    RequireText(Type(composer, L"Google"), L"Google", "live conversion shows them as typed");
    RequireText(composer.Commit(), L"Google", "and commits them");
}

// Ctrl+Backspace right after committing takes the text back.
void TestUndoCommit(const RomajiTable& table, const MiniPack& pack) {
    using tekito::japanese::KeyCommand;
    using tekito::japanese::KeyPress;
    using Key = KeyPress::Key;
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    const tekito::japanese::KeyOptions options;
    KeyPress undo;
    undo.key = Key::Backspace;
    undo.command = true;
    undo.control = true;

    Require(!tekito::japanese::TranslateKey(composer, undo, options), "nothing to take back at first");
    Type(composer, L"watashinonamaehanakanodesu");
    composer.Convert();
    composer.MoveFocus(2);
    composer.NextCandidate();
    const std::wstring shown = composer.Preedit();
    RequireText(composer.Commit(), shown, "Enter commits");
    const auto command = tekito::japanese::TranslateKey(composer, undo, options);
    Require(command && command->action == KeyCommand::Action::UndoCommit, "Ctrl+Backspace takes it back");
    const auto outcome = tekito::japanese::ApplyKey(composer, *command);
    Require(outcome.uncommitted == shown, "the text to take out of the document");
    Require(composer.IsConverted(), "back to the conversion");
    RequireText(composer.Preedit(), shown, "with the candidates as chosen");
    RequireText(SegmentsText(composer).substr(SegmentsText(composer).rfind(L'|') + 1, 1), L"*", "and the focus where it was");
    Require(!composer.LastCommit(), "only once");

    (void)composer.Commit();
    Type(composer, L"a");
    Require(!composer.LastCommit(), "typing forgets it");
    composer.Clear();
    Type(composer, L"nakano");
    (void)composer.Commit();
    Require(composer.UndoCommit() && composer.Preedit() == L"なかの" && !composer.IsConverted(),
            "kana committed as typed come back as typed");
}

// Reconversion: written text back in the composition.
void TestReconvert(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    const auto guess = converter.ReadingOf(L"私の名前は中野です");
    Require(guess && guess->reading == L"わたしのなまえはなかのです", "the reading of written text");
    Require(!converter.ReadingOf(L"鬱"), "nothing for a kanji in no word");
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    Require(composer.Reconvert(L"私の名前は中野です"), "reconverts");
    Require(composer.IsConverted(), "as a conversion");
    RequireText(composer.Preedit(), L"私の名前は中野です", "showing the text as written");
    composer.Cancel();
    RequireText(composer.Preedit(), L"わたしのなまえはなかのです", "Esc gives its kana");
    composer.Clear();
    Require(composer.Reconvert(L"なかの") && composer.IsConverted(), "kana reconvert too");
    composer.Clear();
    tekito::japanese::KeyPress henkan;
    henkan.key = tekito::japanese::KeyPress::Key::Convert;
    const auto command = tekito::japanese::TranslateKey(composer, henkan, {});
    Require(command && command->action == tekito::japanese::KeyCommand::Action::Reconvert,
            "Henkan with nothing typed asks for reconversion");
}

std::vector<tekito::japanese::PhraseCandidate> Candidates(std::initializer_list<const wchar_t*> texts) {
    std::vector<tekito::japanese::PhraseCandidate> out;
    for (const auto* text : texts) out.push_back({text, 0, tekito::japanese::PhraseCandidate::Kind::Dictionary, false});
    return out;
}

void TestLearningStore() {
    tekito::japanese::JapaneseLearningStore learning;
    auto candidates = Candidates({L"機会", L"機械", L"器械"});
    learning.Reorder(L"きかい", candidates);
    RequireText(candidates[0].text, L"機会", "nothing learned keeps the order");

    learning.RecordChoice(L"きかい", L"機械", L"機会");
    learning.Reorder(L"きかい", candidates);
    RequireText(candidates[0].text, L"機械", "a choice moves to the top");
    RequireText(candidates[1].text, L"機会", "the rest keep their order");

    for (int i = 0; i < 4; ++i) learning.RecordChoice(L"きかい", L"機械", L"機械");
    learning.RecordChoice(L"きかい", L"器械", L"機械");
    learning.Reorder(L"きかい", candidates);
    RequireText(candidates[0].text, L"機械", "one different choice does not displace a habit");
    RequireText(candidates[1].text, L"器械", "the new choice comes second");

    auto other = Candidates({L"機会", L"機械"});
    learning.Reorder(L"きかいが", other);
    RequireText(other[0].text, L"機会", "learning is per reading");

    tekito::japanese::JapaneseLearningStore copy;
    for (auto entry : learning.Entries()) Require(copy.Add(entry), "entries load back");
    Require(copy.Preference(L"きかい", L"機械") == learning.Preference(L"きかい", L"機械"),
            "loaded entries keep their preference");
    Require(!copy.Add({L"", L"x", 1, 0, 1}), "an entry without a reading is refused");
    copy.Clear();
    Require(copy.Empty(), "clearing forgets everything");
}

void TestLoanwords(const RomajiTable& table, const MiniPack& pack) {
    using tekito::japanese::LoanwordKey;
    RequireText(LoanwordKey(L"ミーティング"), L"みいちんぐ", "long vowels are written out, ティ is チ");
    RequireText(LoanwordKey(L"ファイル"), L"ふあいる", "small vowels are full size");

    tekito::japanese::Loanwords loanwords;
    Require(loanwords.Open(tekito::ExternalLexiconProvider::PackDirectory(L"japanese-loanwords")),
            "the loanwords pack opens");
    const auto meeting = loanwords.Words(L"ミーティング", 2);
    Require(!meeting.empty() && meeting.front() == L"meeting", "ミーティング is meeting");
    Require(loanwords.Words(L"キカイ", 2).empty(), "katakana that is no English word has none");

    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetLoanwords(&loanwords);
    Type(composer, L"mi-thingugahajimaru");
    composer.Convert();
    const auto* candidates = composer.FocusedCandidates();
    Require(candidates && candidates->front().text == L"ミーティングが", "katakana stays the first choice");
    Require(candidates->size() > 1 && (*candidates)[1].text == L"meetingが" &&
                (*candidates)[1].kind == tekito::japanese::PhraseCandidate::Kind::English,
            "the English word comes right after it, with what follows");
    composer.Clear();

    // Romaji that happens to spell English stays Japanese: keys are not
    // read as English.
    Type(composer, L"kikaigatomaru");
    composer.Convert();
    for (const auto& segment : composer.Segments()) {
        Require(std::none_of(segment.text.begin(), segment.text.end(), [](wchar_t c) { return c < 0x80; }),
                "no English in a Japanese sentence");
    }
}

// Mixed typing: each chunk before Space is English or Japanese.
void TestMixedTyping(const RomajiTable& table, const MiniPack& pack) {
    struct WordScores final : tekito::IFrequencyProvider {
        double Score(std::wstring_view word) const noexcept override {
            for (const auto& [listed, score] : {std::pair<std::wstring_view, double>{L"github", 2.7},
                                                {L"push", 4.2}, {L"to", 6.7}, {L"take", 5.1}, {L"is", 6.3}, {L"i", 7.5},
                                                {L"don't", 4.6}, {L"home", 4.5}}) {
                if (word == listed) return score;
            }
            return 0.0;
        }
    } scores;
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    const tekito::japanese::ChunkJudge judge(table, converter, nullptr, scores, nullptr);
    Require(judge.Judge(L"github", 0).english, "keys that make no kana and are a word are English");
    Require(judge.Judge(L"push", 0).english, "push is English");
    Require(judge.Judge(L"chatgpt", 0).english, "keys that make no kana and no Japanese are English");
    Require(!judge.Judge(L"to", 0).english, "two letters are a particle");
    Require(!judge.Judge(L"to", 1).english, "after one English part too (github ni)");
    Require(judge.Judge(L"to", 2).english, "in an English sentence, to is English");
    Require(!judge.Judge(L"kikai", 0).english, "romaji that is no English word is Japanese");
    Require(judge.Judge(L"kikai", 2, false, nullptr, L"push is ").english,
            "an English sentence with grammar in it goes on in English");
    Require(!judge.Judge(L"kikai", 2, false, nullptr, L"github push ").english,
            "English nouns and verbs alone do not make one");
    Require(!judge.Judge(L"pcwotukau", 0).english, "English run into Japanese after a particle is Japanese");
    const auto numbered = judge.Judge(L"github15", 0);
    Require(numbered.english && numbered.written == L"github15", "a number after a word keeps the word English");
    const auto particle = judge.Judge(L"dr", 1);
    Require(!particle.english && particle.meant == L"de", "two keys after English one slip from a particle are it");
    Require(judge.Judge(L"ggithub", 0).english, "a first letter typed twice is a slip of the English word");
    Require(judge.Judge(L"don't", 0).english, "a contraction is English");
    {
        const auto english = judge.JudgeSentence({L"i", L"push", L"to", L"home"});
        Require(std::all_of(english.begin(), english.end(), [](const auto& j) { return j.english; }),
                "an English sentence is English throughout (to, home: not と, 褒め)");
        const auto mixed = judge.JudgeSentence({L"kikai", L"to", L"push", L"home"});
        Require(!mixed.front().english, "a chunk that is only Japanese keeps the sentence as judged");
    }
    const auto pieces = judge.Split(L"pushsimasita");
    Require(pieces && pieces->size() == 2 && (*pieces)[0].english && (*pieces)[0].text == L"push" && !(*pieces)[1].english,
            "English run into Japanese without a space is split off");
    Require(!judge.Split(L"pushin"), "two keys after it that are no particle are not Japanese");
    {
        JapaneseComposer brackets(&table);
        const auto pair = brackets.BracketPair(L'[');
        Require(pair && pair->first == L"「" && pair->second == L"」", "[ opens 「 and is closed by 」");
        Require(!brackets.BracketPair(L'a'), "a letter opens no bracket");
    }

    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetMixedTyping(&judge);
    Type(composer, L"push");
    composer.Convert();
    RequireText(composer.Commit(), L"Push", "English that starts a sentence gets a capital, and no space after");
    composer.ForgetContext();
    composer.SetSentenceStart(false);
    Type(composer, L"Kikai");
    composer.Convert();
    RequireText(composer.Commit(), L"Kikai", "a capital typed in a chunk makes it English, as typed");
    Type(composer, L"push.");
    composer.Convert();
    RequireText(composer.Commit(), L" push.", "punctuation after English stays English; English after English is spaced");
    Type(composer, L"push.");
    RequireText(composer.Commit(), L" Push.", "and so on Enter (a new sentence)");
    composer.ForgetContext();
    composer.SetSentenceStart(true);
    Type(composer, L"push");
    composer.Convert();
    const auto converted = composer.Segments();
    Require(converted.size() == 2 && converted.back().text == L" " &&
                composer.CaretOffset() == converted.front().text.size() + 1,
            "English converted with Space shows the space after it, the caret past it");
    Type(composer, L"kikai");
    const auto held = composer.Segments();
    Require(held.size() == 2 && held.front().settled && held.front().text == L"Push ",
            "Mix holds the chunks before the one typed until Enter");
    composer.Convert();
    const auto sentence = composer.Commit();
    Require(sentence.starts_with(L"Push") && sentence.size() > 4 && sentence[4] >= 0x3000,
            "Enter commits the whole sentence, Japanese right after English");
    composer.ForgetContext();
    composer.SetSentenceStart(false);
    Type(composer, L"github");
    composer.Convert();
    const auto* candidates = composer.FocusedCandidates();
    Require(candidates && candidates->front().text == L"github" &&
                candidates->front().kind == tekito::japanese::PhraseCandidate::Kind::English,
            "an English chunk converts to itself");
    RequireText(composer.Commit(), L"github", "and is committed so");
    Type(composer, L"push");
    composer.Convert();
    RequireText(composer.Commit(), L" push", "English after English takes a space before it");
    Type(composer, L"kikai");
    composer.Convert();
    const auto japanese = composer.Commit();
    Require(!japanese.empty() && japanese.front() >= 0x3000 && !composer.TakeSpaceBefore(),
            "Japanese after English takes none, and no space is left to take back");
    Type(composer, L"push");
    RequireText(composer.Commit(), L"push", "Enter takes an English chunk as Space would");
    composer.ForgetContext();
    Type(composer, L"kikai");
    RequireText(composer.Commit(), L"きかい", "Enter converts nothing: a Japanese chunk is committed as typed");
    {
        // Mix live conversion: Japanese shows converted as it is typed;
        // English stays as typed.
        composer.ForgetContext();
        composer.SetMixLiveConversion(true);
        Type(composer, L"kikai");
        const std::wstring live = SegmentsText(composer);
        composer.Cancel();
        composer.Clear();
        Type(composer, L"push");
        const std::wstring english = SegmentsText(composer);
        composer.Clear();
        composer.SetMixLiveConversion(false);
        Type(composer, L"kikai");
        const std::wstring off = SegmentsText(composer);
        composer.Clear();
        Type(composer, L"push");
        const std::wstring englishOff = SegmentsText(composer);
        composer.Clear();
        Require(!live.empty() && live.front() >= 0x4E00 && english == englishOff && off == L"きかい",
                "Mix live conversion shows Japanese converted, English as typed, and is off by default");
    }

    {
        // A vowel typed alone stays kana when the sentence is read again,
        // unless an English word is beside it.
        const auto typed = [&](std::initializer_list<std::wstring_view> chunks) {
            composer.ForgetContext();
            composer.SetSentenceStart(true);
            for (const auto chunk : chunks) {
                Type(composer, chunk);
                composer.Convert();
            }
            Type(composer, L"kikai");
            const auto shown = composer.Segments();
            (void)composer.Commit();
            composer.ForgetContext();
            return shown.empty() ? std::wstring() : shown.front().text;
        };
        const auto kana = typed({L"i", L"ga"});
        const auto letter = typed({L"i", L"push"});
        composer.SetSingleVowelKana(false);
        const auto letterOff = typed({L"i", L"push"});
        composer.SetSingleVowelKana(true);
        Require(kana.starts_with(L"い"), "i before Japanese stays い");
        Require(!letter.empty() && letter == letterOff, "i before an English word is read as the sentence reads it");
    }

    using tekito::japanese::JapaneseComposer;
    RequireText(JapaneseComposer::SpacedEnglish(L"", L"\u4ECA\u65E5\u306FGitHub\u306Bpush\u3057\u305F"),
                L"\u4ECA\u65E5\u306F GitHub \u306B push \u3057\u305F", "spaces between Japanese and English");
    RequireText(JapaneseComposer::SpacedEnglish(L"GitHub", L"\u306B"), L" \u306B", "and against the text before");
    RequireText(JapaneseComposer::SpacedEnglish(L"", L"3\u6642\u3001iPhone15\u3092"), L"3\u6642\u3001iPhone15 \u3092",
                "not by digits alone or punctuation");
    RequireText(JapaneseComposer::SpacedEnglish(L"", L"I want to go"), L"I want to go", "English stays");
    composer.SetMixedTyping(nullptr);
    Type(composer, L"push");
    composer.Convert();
    Require(composer.Preedit() != L"push", "without mixed typing the keys are romaji");
    composer.Clear();
}

void TestRomajiCorrection(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    // Slips are corrected in the key lattice, English words or not.
    const tekito::japanese::KeyConverter keyConverter(pack.dictionary, pack.matrix, converter, table);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetKeyConverter(&keyConverter);

    RequireText(Type(composer, L"arigatpu"), L"ありがｔぷ", "a slipped key leaves a letter");
    composer.Convert();
    const auto fixed = composer.Preedit();
    Require(fixed.find(L'ｔ') == std::wstring::npos && fixed.find(L"ありがと") != std::wstring::npos,
            "Space converts the corrected reading ありがとう");
    composer.Cancel();
    RequireText(composer.Preedit(), L"ありがｔぷ", "Esc goes back to what was typed");
    composer.Clear();

    Type(composer, L"sjigoto");
    composer.Convert();
    Require(composer.Preedit().find(L"仕事") != std::wstring::npos, "sjigoto converts as しごと");
    composer.Clear();

    // A letter left last is meant: no slip drops or changes it.
    for (const auto* keys : {L"ok", L"arigatouk", L"kk"}) {
        Type(composer, keys);
        const auto typed = composer.Preedit();
        composer.Convert();
        Require(!typed.empty() && composer.Preedit().ends_with(typed.back()), "Space keeps a letter typed last");
        composer.Clear();
    }

    // Slips that still read as kana: a neighboring key, a key dropped, an
    // extra key, two keys swapped.
    for (const auto* keys : {L"arigayougozaimasu", L"arigtougozaimasu", L"arigatouggozaimasu",
                             L"arigatougozaimsau"}) {
        RequireText(Type(composer, keys).substr(0, 2), L"あり", "the slip is typed as it is");
        composer.Convert();
        RequireText(composer.Preedit(), L"ありがとうございます", "Space converts what was meant");
        composer.Clear();
    }
    // The keys as typed stay a choice, second.
    Type(composer, L"arigayougozaimasu");
    composer.Convert();
    {
        const auto* list = composer.FocusedCandidates();
        Require(list && list->size() > 1 && (*list)[1].reading == L"ありがようございます",
                "after a slip undone, the keys as typed are the second choice");
    }
    composer.Clear();
    Type(composer, L"shigotogaowatta");
    composer.Convert();
    const std::wstring meant = composer.Preedit();
    composer.Clear();
    Type(composer, L"shigotogaowqtta");
    composer.Convert();
    RequireText(composer.Preedit(), meant, "a slip inside a sentence");
    composer.Clear();
    // The first key typed twice: a phrase does not start with っ, but a
    // short input may be meant so.
    Type(composer, L"sshigotoga");
    composer.Convert();
    RequireText(composer.Preedit().substr(0, 2), L"仕事", "sshigotoga converts as しごとが");
    composer.Clear();
    Type(composer, L"kka");
    composer.Convert();
    RequireText(composer.Preedit().substr(0, 1), L"っ", "kka stays っか");
    composer.Clear();

    // A dropped consonant (ikaiga: kikaiga) is undone when the words read
    // better so; the other spelling is one pick away.
    Type(composer, L"ikaigatomaru");
    composer.Convert();
    const std::wstring first = composer.Preedit();
    composer.NextCandidate();
    const auto* listed = composer.FocusedCandidates();
    Require(first.starts_with(L"\u6A5F"), "ikaiga converts as きかいが");
    Require(std::any_of(listed->begin(), listed->end(),
                        [](const tekito::japanese::PhraseCandidate& c) { return c.text.starts_with(L"機械が"); }),
            "the list offers 機械が for ikaiga");
    composer.Clear();

    // A slip that split a short input into phrases: the whole input read
    // again is in the list, and choosing it makes one phrase of it.
    Type(composer, L"hahimemashite");
    composer.Convert();
    Require(composer.Segments().size() > 1, "the slip splits hahimemashite into phrases");
    composer.NextCandidate();
    const auto* phraseList = composer.FocusedCandidates();
    const auto whole = std::find_if(phraseList->begin(), phraseList->end(),
                                    [](const tekito::japanese::PhraseCandidate& c) { return c.text == L"はじめまして"; });
    Require(whole != phraseList->end() && whole->slip && whole->reading == L"はじめまして",
            "the first phrase's list has the whole input read again");
    Require(whole - phraseList->begin() < 9, "on the list's first page");
    const auto wholeAt = static_cast<std::size_t>(whole - phraseList->begin());
    composer.SelectCandidate(wholeAt);
    composer.MoveFocus(1);
    Require(composer.Segments().size() > 1 && composer.Preedit().find(L"はじめまして") == std::wstring::npos,
            "Right from the whole input shows the phrases, not the whole input and the phrases after it");
    composer.MoveFocus(-1);
    composer.SelectCandidate(wholeAt);
    RequireText(composer.Preedit(), L"はじめまして", "choosing it shows the whole input as it");
    Require(composer.Segments().size() == 1, "as one phrase");
    RequireText(composer.Commit(), L"はじめまして", "and commits it");
    composer.Clear();

    Type(composer, L"thnaks");
    composer.Convert();
    Require(composer.Preedit().find(L'ｔ') != std::wstring::npos, "many unreadable letters stay as typed");
    composer.Clear();

    Type(composer, L"kikai");
    composer.Convert();
    Require(composer.Preedit().find(L"機") != std::wstring::npos, "correct romaji is not touched");
    composer.Clear();

    // Off: Space keeps the reading as typed; the list may still offer the fix.
    composer.SetSlipAutoApply(false);
    Type(composer, L"arigatpu");
    composer.Convert();
    Require(composer.Preedit().find(L'ｔ') != std::wstring::npos,
            "with slip auto-apply off, Space keeps the typed reading");
    composer.Clear();
    composer.SetSlipAutoApply(true);

    // After a slip is undone, F10 and typed forms still expose the keys typed.
    Type(composer, L"arigatpu");
    composer.Convert();
    Require(composer.Preedit().find(L'ｔ') == std::wstring::npos, "Space undoes the slip");
    composer.Transliterate(KanaForm::HalfWidthAlphanumeric);
    RequireText(composer.Preedit(), L"arigatpu", "F10 after a slip shows the keys typed");
    composer.Clear();
    Type(composer, L"arigatpu");
    composer.Convert();
    composer.NextCandidate();
    const auto* afterSlip = composer.FocusedCandidates();
    Require(afterSlip && std::any_of(afterSlip->begin(), afterSlip->end(),
                                     [](const tekito::japanese::PhraseCandidate& c) { return c.text == L"arigatpu"; }),
            "typed forms still list the keys after a slip");
    composer.Clear();
}

void TestPrediction(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    const auto predicted = converter.Predict(L"ありが", 5);
    Require(!predicted.empty() && predicted.front().reading.starts_with(L"ありが") &&
                predicted.front().reading.size() > 3,
            "words that start with ありが are predicted");

    tekito::japanese::JapaneseLearningStore learning;
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetLearning(&learning);
    Type(composer, L"a");
    Require(composer.Predictions().empty(), "prediction is off by default");
    composer.Clear();

    composer.SetPredictionEnabled(true);
    Type(composer, L"a");
    Require(composer.Predictions().empty(), "one kana predicts nothing");
    Type(composer, L"riga");
    Require(!composer.Predictions().empty(), "ありが predicts words");
    Require(!composer.ChosenPrediction(), "nothing is chosen until Tab");
    RequireText(composer.Preedit(), L"ありが", "predictions do not change the text");
    composer.NextPrediction();
    Require(composer.ChosenPrediction() == 0u, "Tab chooses the first prediction");
    composer.PreviousPrediction();
    Require(composer.ChosenPrediction() == composer.Predictions().size() - 1, "Up wraps to the last");
    composer.ChoosePrediction(0);
    const auto chosen = composer.Predictions().front().text;
    RequireText(composer.Commit(), chosen, "Enter commits the chosen prediction");

    Type(composer, L"ariga");
    RequireText(composer.Predictions().front().text, chosen, "a prediction chosen before comes first");
    Type(composer, L"k");
    Require(composer.Predictions().empty(), "a pending key hides predictions");
    composer.Backspace();
    Require(!composer.Predictions().empty(), "Backspace brings them back");
    composer.Convert();
    Require(composer.Predictions().empty(), "converting puts predictions away");
    composer.Clear();

    // Live: a pause converts again but keeps the predictions for Tab.
    composer.SetLiveConversion(true);
    Type(composer, L"ariga");
    Require(!composer.Predictions().empty(), "live: ありが predicts words");
    (void)composer.RefineLive();
    Require(!composer.Predictions().empty(), "live: a pause keeps the predictions");
    composer.NextPrediction();
    const auto pickedLive = composer.Predictions()[*composer.ChosenPrediction()].text;
    RequireText(composer.Commit(), pickedLive, "live: Enter commits the prediction Tab picked after the pause");
    composer.SetLiveConversion(false);

    // Mix: Enter takes the prediction picked with Tab, not a conversion.
    struct NoWords final : tekito::IFrequencyProvider {
        double Score(std::wstring_view) const noexcept override { return 0.0; }
    } noWords;
    const tekito::japanese::ChunkJudge judge(table, converter, nullptr, noWords, nullptr);
    composer.SetMixedTyping(&judge, 0);
    Type(composer, L"ariga");
    composer.NextPrediction();
    const auto picked = composer.Predictions()[*composer.ChosenPrediction()].text;
    RequireText(composer.Commit(), picked, "Mix: Enter commits the prediction chosen with Tab");
    Type(composer, L"ariga");
    composer.Convert();
    Type(composer, L"ariga");
    composer.NextPrediction();
    const auto second = composer.Predictions()[*composer.ChosenPrediction()].text;
    const auto sentence = composer.Commit();
    Require(sentence.ends_with(second), "after another chunk too");
    composer.SetMixedTyping(nullptr, 0);
}

void TestUserWords(const RomajiTable& table, const MiniPack& pack) {
    using tekito::japanese::JapaneseUserDictionary;
    using tekito::japanese::UserPartsOfSpeech;
    using tekito::japanese::UserWordKind;

    const auto parsed = UserPartsOfSpeech::Parse("# kind\tleft\tright\tcost\nnoun\t12\t13\t4000\r\nbogus\t1\t1\t1\nplace\tx\t1\t1\n");
    Require(parsed.For(UserWordKind::Noun).has_value() && parsed.For(UserWordKind::Noun)->left == 12 &&
                parsed.For(UserWordKind::Noun)->right == 13 && !parsed.For(UserWordKind::Place),
            "pos.tsv rows are read; unknown kinds and bad numbers are skipped");
    Require(tekito::japanese::KindFromName(tekito::japanese::KindName(UserWordKind::GivenName)) ==
                UserWordKind::GivenName,
            "a kind's name reads back as the kind");

    const auto parts = UserPartsOfSpeech::Load(std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-mini" / L"pos.tsv");
    Require(parts.For(UserWordKind::Surname).has_value() && parts.For(UserWordKind::Symbol).has_value(),
            "the test pack has a part of speech for each kind");

    JapaneseUserDictionary none;
    none.Set({{L"なかの", L"中埜", UserWordKind::Surname}}, UserPartsOfSpeech{});
    Require(none.Empty(), "without parts of speech no user word is used");

    JapaneseUserDictionary user;
    user.Set({{L"なかの", L"中埜", UserWordKind::Surname},
              {L"てきとう", L"TEKITO", UserWordKind::ProperNoun},
              {L"てきとう", L"TEKITO", UserWordKind::ProperNoun}},
             parts);
    Require(user.Size() == 2, "a word added twice is kept once");

    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    const auto plain = converter.Convert(L"なかの");
    Require(!plain.empty() && plain.front().candidates.front().text != L"中埜",
            "without the user's word なかの converts as the dictionary has it");
    const auto withUser = converter.Convert(L"なかの", {}, 0, {}, &user);
    Require(withUser.size() == 1, "なかの stays one phrase");
    RequireText(withUser.front().candidates.front().text, L"中埜", "the user's word comes first for its reading");
    Require(HasCandidate(withUser.front(), plain.front().candidates.front().text, 8),
            "the dictionary's words are still offered");

    const auto best = converter.Best(L"てきとう", 0, &user);
    Require(best && best->text == L"TEKITO", "a word the dictionary lacks converts from the user's words");
    const auto predicted = converter.Predict(L"てき", 5, &user);
    Require(!predicted.empty() && predicted.front().text == L"TEKITO", "the user's words are predicted first");

    // Offered, not first; and never offered.
    const auto dictionaryFirst = plain.front().candidates.front().text;
    JapaneseUserDictionary offered;
    offered.Set({{L"なかの", L"中埜", UserWordKind::Surname, tekito::japanese::UserWordAction::Suggest}}, parts);
    const auto withOffer = converter.Convert(L"なかの", {}, 0, {}, &offered);
    Require(withOffer.front().candidates.size() > 1 && withOffer.front().candidates[0].text == dictionaryFirst &&
                withOffer.front().candidates[1].text == L"中埜",
            "a word only to offer comes second and the conversion stays");
    const auto offeredPredictions = converter.Predict(L"なか", 5, &offered);
    Require(std::any_of(offeredPredictions.begin(), offeredPredictions.end(),
                        [](const tekito::japanese::Prediction& p) { return p.text == L"中埜"; }),
            "a word only to offer is predicted too");

    JapaneseUserDictionary hidden;
    hidden.Set({{L"なかの", dictionaryFirst, UserWordKind::Noun, tekito::japanese::UserWordAction::Suppress}}, parts);
    const auto withHidden = converter.Convert(L"なかの", {}, 0, {}, &hidden);
    Require(!withHidden.empty() && !HasCandidate(withHidden.front(), dictionaryFirst, 40),
            "a suppressed word is never offered for its reading");
    const auto hiddenBest = converter.Best(L"なかの", 0, &hidden);
    Require(!hiddenBest || hiddenBest->text != dictionaryFirst, "nor chosen as the likeliest");

    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetUserDictionary(&user);
    Type(composer, L"tekitou");
    composer.Convert();
    RequireText(composer.Preedit(), L"TEKITO", "the composer converts to the user's word");
    composer.Clear();
    composer.SetUserDictionary(nullptr);
    Type(composer, L"nakano");
    composer.Convert();
    Require(composer.Preedit() != L"中埜", "without the user dictionary the word is gone");
}

void TestContext(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    Require(composer.Context() == 0, "typing starts at the start of a sentence");
    Type(composer, L"watashino");
    composer.Convert();
    RequireText(composer.Commit(), L"私の", "the first phrase commits");
    Require(composer.Context() != 0, "what was committed is where the next conversion starts");
    const auto after = composer.Context();
    Type(composer, L"namaeha");
    composer.Convert();
    const auto* candidates = composer.FocusedCandidates();
    const auto alone = converter.Convert(L"なまえは");
    const auto followed = converter.Convert(L"なまえは", {}, after);
    Require(candidates && !alone.empty() && !followed.empty() &&
                candidates->front().cost == followed.front().candidates.front().cost,
            "the next phrase is converted as following it");
    (void)composer.Commit();
    composer.ForgetContext();
    Require(composer.Context() == 0, "forgetting starts afresh");
    Type(composer, L"kikai.");
    composer.Convert();
    (void)composer.Commit();
    Require(composer.Context() == 0, "a sentence that ended starts the next afresh");
}

void TestLanguageModel(const MiniPack& pack) {
    using tekito::japanese::LanguageModel;
    LanguageModel model;
    Require(model.Open(std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-lm-mini"), "the model opens");
    // The builder's hashes and these meet (scripts/ja/build-japanese-lm.py --mini).
    const auto count = model.LogCount(L"麻酔");
    Require(count && std::abs(*count - std::log(1000.0)) < 0.06, "word counts are read back");
    Require(!model.LogCount(L"xyz"), "a word the model lacks has none");
    const auto pmi = model.PairPmi(LanguageModel::PairStart(L"よろしく"), L"お願い");
    Require(pmi && std::abs(*pmi - 5.0) < 0.06, "pairs are read back");
    Require(!model.PairPmi(LanguageModel::PairStart(L"お願い"), L"よろしく"), "pairs have an order");
    const auto tie = model.Topic(L"麻酔", L"注射");
    Require(tie && std::abs(*tie - std::log(41.0)) < 0.06 && model.Topic(L"注射", L"麻酔") == tie,
            "sentence ties are read back, either way round");
    Require(!model.Topic(L"麻酔", L"駐車"), "words that do not share sentences have none");
    Require(LanguageModel::IsContentWord(L"注射") && !LanguageModel::IsContentWord(L"を") &&
                !LanguageModel::IsContentWord(L"する"),
            "content words have kanji or katakana");

    tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    converter.SetLanguageModel(&model);
    const auto phrases = converter.Convert(L"ますいをちゅうしゃする");
    Require(!phrases.empty(), "the sentence converts");
    std::wstring first;
    for (const auto& phrase : phrases) first += phrase.candidates.front().text;
    RequireText(first, L"麻酔を注射する", "the sentence's other words pick 注射 over 駐車");

    // Typed phrase by phrase: what was committed before counts too.
    const auto table = LoadTable();
    JapaneseComposer composer(table.get());
    composer.SetConverter(&converter);
    Type(composer, L"masuiwo");
    composer.Convert();
    RequireText(composer.Commit(), L"麻酔を", "the first phrase commits");
    Type(composer, L"chuushasuru");
    composer.Convert();
    RequireText(composer.Preedit(), L"注射する", "the committed 麻酔 picks 注射 for the next phrase");
    composer.Clear();
    composer.ForgetContext();
    Type(composer, L"chuushasuru");
    composer.Convert();
    Require(composer.FocusedCandidates() != nullptr, "without it the phrase still converts");

    LanguageModel damaged;
    Require(!damaged.Open(std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-mini"),
            "a folder without the model opens nothing");
}

void TestComposerLearning(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    tekito::japanese::JapaneseLearningStore learning;
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetLearning(&learning);

    Type(composer, L"kikaigatomaru");
    composer.Convert();
    const auto first = composer.Segments().front().text;
    const auto* candidates = composer.FocusedCandidates();
    std::size_t other = 1;
    while (other < candidates->size() && (*candidates)[other].text == first) ++other;
    const auto chosen = (*candidates)[other].text;
    composer.SelectCandidate(other);
    (void)composer.Commit();

    Type(composer, L"kikaigatomaru");
    composer.Convert();
    RequireText(composer.Segments().front().text, chosen, "the phrase chosen last time comes first");
    Require(!composer.CanForgetChosen(), "Ctrl+Delete needs the list open");
    composer.Convert();
    composer.PreviousCandidate();
    Require(composer.CanForgetChosen(), "a learned candidate can be forgotten");
    tekito::japanese::KeyPress forget;
    forget.key = tekito::japanese::KeyPress::Key::Delete;
    forget.command = true;
    forget.control = true;
    const auto command = tekito::japanese::TranslateKey(composer, forget, {});
    Require(command && command->action == tekito::japanese::KeyCommand::Action::ForgetChosen, "with Ctrl+Delete");
    (void)tekito::japanese::ApplyKey(composer, *command);
    composer.Cancel();
    composer.Clear();
    Type(composer, L"kikaigatomaru");
    composer.Convert();
    RequireText(composer.Segments().front().text, first, "forgotten, the converter's order is back");
    composer.Cancel();
    composer.Clear();

    learning.RecordChoice(L"なかのです", L"中野デス", L"中野です");
    composer.SetPredictionEnabled(true);
    Type(composer, L"nakano");
    composer.NextPrediction();
    Require(composer.Predictions().front().text == L"中野デス" && composer.CanForgetChosen(), "a learned prediction");
    Require(composer.ForgetChosen(), "Ctrl+Delete forgets it");
    Require(composer.Predictions().empty() || composer.Predictions().front().text != L"中野デス", "and it leaves the list");
    Require(!learning.Contains(L"なかのです", L"中野デス"), "and the learning");
    composer.SetPredictionEnabled(false);
    composer.Clear();

    composer.SetLearning(nullptr);
    Type(composer, L"kikaigatomaru");
    composer.Convert();
    RequireText(composer.Segments().front().text, first, "without learning the order is the converter's");
}

void TestKanjiSets() {
    using tekito::japanese::KanjiRange;
    tekito::japanese::KanjiSets sets;
    sets.Add(U'本', 1);  // 教育, 1年
    sets.Add(U'鬱', 8);  // 常用
    sets.Add(U'桜', 5);  // 教育, 5年
    sets.Add(U'櫻', 9);  // 人名用
    Require(sets.Allows(L"薔薇", KanjiRange::All), "all: any kanji");
    Require(!sets.Allows(L"薔薇", KanjiRange::JoyoAndJinmei) && sets.Allows(L"櫻", KanjiRange::JoyoAndJinmei),
            "常用・人名用: 人名用 kanji, not others");
    Require(!sets.Allows(L"櫻", KanjiRange::Joyo) && sets.Allows(L"憂鬱", KanjiRange::Joyo) == false &&
                sets.Allows(L"鬱", KanjiRange::Joyo),
            "常用: not 人名用, nor kanji outside the sets");
    Require(!sets.Allows(L"鬱", KanjiRange::Kyoiku) && sets.Allows(L"本を読むゆううつ", KanjiRange::Kyoiku) == false &&
                sets.Allows(L"本をよむ、ホン abc", KanjiRange::Kyoiku),
            "教育: only 教育 kanji; kana, letters and marks always");
    Require(!sets.Allows(L"𠮟", KanjiRange::JoyoAndJinmei), "kanji outside the BMP are checked too");
    Require(sets.Allows(L"本", KanjiRange::Kyoiku, 1) && !sets.Allows(L"桜", KanjiRange::Kyoiku, 4) &&
                sets.Allows(L"桜", KanjiRange::Kyoiku, 5),
            "教育: up to a school year");
    Require(sets.Allows(L"本櫻", KanjiRange::Kyoiku, 1, true) && !sets.Allows(L"鬱", KanjiRange::Kyoiku, 6, true) &&
                sets.Allows(L"鬱櫻", KanjiRange::Joyo, 6, true),
            "人名用 kanji added to 教育 or 常用");
}

void TestMeanings() {
    tekito::japanese::MeaningDictionary meanings;
    Require(meanings.Open(std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"meanings-mini"), "the meaning packs open");
    const auto first = [&](std::wstring_view text, std::wstring_view reading) {
        const auto meaning = meanings.Lookup(text, reading);
        return meaning ? meaning->headword + L":" + meaning->senses.front() : std::wstring(L"-");
    };
    RequireText(first(L"私", L"わたし"), L"私:自分自身のことを指す一人称。", "the reading picks the row");
    RequireText(first(L"私", L""), L"私:一人称。", "without a reading, the row that names none");
    RequireText(first(L"私の", L"わたしの"), L"私:自分自身のことを指す一人称。", "particles come off");
    RequireText(first(L"会った", L"あった"), L"会う:人と同じ場所で時間を共有する。",
                "an inflected verb finds its dictionary form, not the single kanji");
    RequireText(first(L"食べました", L"たべました"), L"食べる:口から嚙んで飲み込む。", "longer inflections too");
    RequireText(first(L"行った", L"いった"), L"行く:ある場所へ移動する。", "the reading tells 行く from 行う");
    RequireText(first(L"機械", L"きかい"), L"機械:仕事をする装置。", "WordNet fills in for Wiktionary");
    RequireText(first(L"はし", L"はし"), L"はし:川に渡した構築物。", "kana words are found as they are");
    RequireText(first(L"はしった", L"はしった"), L"-", "but kana is never cut into another word");
    const auto english = meanings.Lookup(L"Meeting");
    Require(english && english->senses.size() == 2 && english->senses[1] == L"the act of coming together",
            "English meanings are split into senses");
    Require(!meanings.Lookup(L"xyzzy") && !meanings.Lookup(L"犬"), "unknown words have no meaning");

    using tekito::japanese::MeaningLanguage;
    const auto in = [&](std::wstring_view text, std::wstring_view reading, MeaningLanguage language) {
        const auto meaning = meanings.Lookup(text, reading, language);
        return meaning ? meaning->headword + L":" + meaning->senses.front() : std::wstring(L"-");
    };
    RequireText(in(L"Meeting", L"", MeaningLanguage::Japanese), L"meeting:会議、ミーティング：公式に準備された集まり",
                "an English word in Japanese");
    RequireText(in(L"会った", L"あった", MeaningLanguage::English), L"会う:meet: come together",
                "a Japanese word in English, from its dictionary form");
    RequireText(in(L"私", L"わたし", MeaningLanguage::English), L"私:自分自身のことを指す一人称。",
                "with no English meaning, the Japanese one");
    RequireText(in(L"Meeting", L"", MeaningLanguage::English), first(L"Meeting", L""),
                "an English word in English is as before");

    tekito::japanese::MeaningDictionary missing;
    Require(!missing.Open(L"Z:/no-such-folder") && !missing.Lookup(L"私"), "missing packs just mean no meanings");
}

}  // namespace

bool Contains(const std::vector<std::wstring>& list, std::wstring_view text) {
    return std::find(list.begin(), list.end(), text) != list.end();
}

void TestSpecialConversions(const RomajiTable& table, const MiniPack& pack) {
    using tekito::LocalTime;
    using tekito::SpecialConversions;
    using Language = SpecialConversions::Language;
    SpecialConversions special;
    Require(special.Load(tekito::ExternalLexiconProvider::PackDirectory(L"special-conversions") / L"rules.tsv"),
            "the special-conversions pack loads");

    const LocalTime morning{2026, 9, 29, 6, 5};  // a Tuesday
    const auto today = special.Dates(Language::English, L"today", morning);
    Require(today.size() >= 3, "today has several formats");
    RequireText(today[0], L"September 29, 2026", "today, written out");
    Require(Contains(today, L"2026-09-29") && Contains(today, L"Tuesday, September 29"), "today as ISO and with the weekday");
    Require(Contains(special.Dates(Language::English, L"now", morning), L"6:05 AM"), "now, in the morning");
    RequireText(special.Dates(Language::English, L"tomorrow", morning)[0], L"September 30, 2026", "tomorrow");
    Require(special.Dates(Language::English, L"table", morning).empty(), "other words have no date");
    const auto heart = special.SymbolsNamed(L"heart");
    Require(heart.size() == 2 && heart[0] == L"\x2661" && heart[1] == L"\x2665", "heart names its symbols, the outline first");
    Require(special.SymbolsNamed(L"table").empty(), "other words name no symbol");

    const auto kyou = special.Dates(Language::Japanese, L"きょう", morning);
    RequireText(kyou[0], L"2026/09/29", "きょう, with slashes");
    Require(Contains(kyou, L"2026年9月29日") && Contains(kyou, L"9月29日(火)") && Contains(kyou, L"令和8年9月29日") &&
                Contains(kyou, L"火曜日"),
            "きょう in kanji, with the weekday and the era");
    RequireText(special.Dates(Language::Japanese, L"あした", {2026, 9, 30, 0, 0})[0], L"2026/10/01",
                "あした goes into the next month");
    Require(Contains(special.Dates(Language::Japanese, L"らいげつ", {2026, 1, 31, 0, 0}), L"2026年2月"),
            "らいげつ from the end of January");
    Require(Contains(special.Dates(Language::Japanese, L"いま", {2026, 9, 29, 13, 5}), L"午後1時5分"),
            "いま in the afternoon");
    Require(Contains(special.Dates(Language::Japanese, L"ことし", {2019, 6, 1, 0, 0}), L"令和元年"),
            "the first year of an era is 元年");

    const auto numbers = special.Numbers(L"1234");
    Require(Contains(numbers, L"１２３４") && Contains(numbers, L"1,234") && Contains(numbers, L"千二百三十四") &&
                Contains(numbers, L"一二三四") && Contains(numbers, L"壱阡弐百参拾四"),
            "a number with commas and in kanji");
    Require(Contains(numbers, L"\x216F\x216D\x216D\x2169\x2169\x2169\x2160\x2164"), "a number in Roman numerals");
    Require(Contains(special.Numbers(L"１２"), L"十二") && Contains(special.Numbers(L"12"), L"\x216B") &&
                Contains(special.Numbers(L"12"), L"\x246B"),
            "twelve in kanji, as one Roman numeral and circled");
    Require(Contains(special.Numbers(L"10000"), L"一万") && Contains(special.Numbers(L"120000000"), L"一億二千万"),
            "large numbers in kanji");
    {
        const auto code = special.Numbers(L"0120");
        Require(code.size() == 3 && Contains(code, L"０１２０") && Contains(code, L"〇一二〇"),
                "a number that starts with 0 changes width or is written a digit at a time, none dropped");
    }
    using tekito::SpecialConversions;
    RequireText(SpecialConversions::Official(L"123456789"), L"1億2,345万6,789", "official style: 億 and 万 with commas");
    RequireText(SpecialConversions::Official(L"120000"), L"12万", "empty groups are left out");
    RequireText(SpecialConversions::Official(L"100002345"), L"1億2,345", "even in the middle");
    RequireText(SpecialConversions::Official(L"1234"), L"1,234", "under 万: commas only");
    Require(Contains(special.Numbers(L"123456789"), L"1億2,345万6,789"), "and it is offered");
    Require(special.Numbers(L"12a").empty(), "not a number");

    RequireText(SpecialConversions::Calculate(L"1+2=").value_or(L""), L"3", "a sum");
    RequireText(SpecialConversions::Calculate(L"（３＋４）＊２＝").value_or(L""), L"14", "full-width arithmetic");
    RequireText(SpecialConversions::Calculate(L"10・4=").value_or(L""), L"2.5", "・ divides, as Japanese input types /");
    RequireText(SpecialConversions::Calculate(L"5ー8＝").value_or(L""), L"-3", "ー subtracts, as Japanese input types -");
    RequireText(SpecialConversions::Calculate(L"2*-3=").value_or(L""), L"-6", "a negative number");
    Require(!SpecialConversions::Calculate(L"1/0=") && !SpecialConversions::Calculate(L"123=") &&
                !SpecialConversions::Calculate(L"1+=") && !SpecialConversions::Calculate(L"1+2"),
            "no sum for dividing by zero, no operator, a missing number or no =");

    Require(Contains(special.Symbols(Language::Japanese, L"やじるし"), L"→"), "やじるし is an arrow");
    Require(Contains(special.Symbols(Language::Japanese, L"ー＞"), L"→"), "-> typed in Japanese is an arrow");
    Require(Contains(special.Symbols(Language::English, L"(c)"), L"©"), "(c) is ©");
    Require(Contains(special.Emoticons(L"にこにこ"), L"(^^)") && special.Emoticons(L"かおもじ").size() > 100,
            "emoticons by reading, and all of them for かおもじ");
    Require(!Contains(special.Symbols(Language::Japanese, L"こ"), L"\U0001B038"), "hentaigana only by their name");
    Require(Contains(special.SingleKanji(L"こ"), L"己") && Contains(special.SingleKanji(L"こ"), L"琥"),
            "single kanji by reading, one each");

    // English, typed outside words.
    const tekito::SpecialConversionOptions all;
    const auto ending = [&](std::wstring_view text) { return special.EnglishEnding(text, all); };
    Require(special.MayEndEnglish(L')', all) && special.MayEndEnglish(L'=', all) && !special.MayEndEnglish(L'a', all),
            "keys that may finish a spelling or a sum");
    Require(ending(L"see (c)") && ending(L"see (c)")->length == 3 && Contains(ending(L"see (c)")->offers, L"©"),
            "(c) at the end of the text is ©");
    Require(ending(L"a <->") && ending(L"a <->")->length == 3, "the longest spelling that fits");
    Require(ending(L"1/2") && !ending(L"11/2") && !ending(L"3/1/2"), "1/2, but not inside a longer number");
    const auto sum = ending(L"I have 3 1 + 2 =");
    Require(sum && sum->length == 7 && sum->offers.size() == 2 && sum->offers[0] == L"3" &&
                sum->offers[1] == L"1 + 2 = 3",
            "a spaced sum, clear of the number before it");
    Require(!ending(L"x=") && !ending(L"a+b=") && !ending(L"hello"), "nothing to offer");
    tekito::SpecialConversionOptions none;
    none.symbols = none.calculator = false;
    Require(!special.EnglishEnding(L"(c)", none) && !special.EnglishEnding(L"1+2=", none) &&
                !special.MayEndEnglish(L')', none),
            "with symbols and the calculator off, nothing");

    // In the composer.
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetSpecialConversions(&special);
    Type(composer, L"1+2=");
    composer.Convert();
    RequireText(composer.Preedit(), L"3", "1+2= converts to the sum");
    Require(composer.FocusedCandidates()->size() >= 2 && (*composer.FocusedCandidates())[1].text == L"1+2=3",
            "and to the arithmetic with the sum");
    composer.Clear();

    Type(composer, L"1234");
    composer.Convert();
    const auto* forms = composer.FocusedCandidates();
    Require(forms && std::any_of(forms->begin(), forms->end(), [](const auto& c) { return c.text == L"千二百三十四"; }),
            "a number's candidates have it in kanji");
    composer.Clear();

    Type(composer, L"yajirusi");
    composer.Convert();
    const auto* arrows = composer.FocusedCandidates();
    Require(arrows && std::any_of(arrows->begin(), arrows->end(), [](const auto& c) { return c.text == L"→"; }),
            "やじるし's candidates have the arrows");
    {
        // The reading's first symbol on the first page; the others after
        // the words.
        const auto arrow = std::find_if(arrows->begin(), arrows->end(), [](const auto& c) { return c.text == L"→"; });
        Require(arrow - arrows->begin() <= 3, "the symbol a reading names is on the first page");
        const auto leftArrow = std::find_if(arrows->begin(), arrows->end(), [](const auto& c) { return c.text == L"←"; });
        Require(leftArrow != arrows->end() &&
                    std::none_of(leftArrow, arrows->end(),
                                 [](const auto& c) { return c.kind == tekito::japanese::PhraseCandidate::Kind::Dictionary; }),
                "the other symbols come after the words");
    }
    composer.Clear();

    // Digits: half-width when asked, decimal points and commas between them,
    // and read as a number with its part of speech, so counters join them.
    {
        JapaneseComposer digits(&table);
        digits.SetHalfWidthDigits(true);
        Type(digits, L"3.14 1,000");
        RequireText(digits.Commit(), L"3.14\x3000" L"1,000", "half-width digits, with a decimal point and a comma");
        RequireText(Committed(table, L"3.14"), L"\xFF13\xFF0E\xFF11\xFF14", "full-width digits by default");
        RequireText(Committed(table, L"3."), L"\xFF13\x3002", "a period after a number ends the sentence");
        JapaneseComposer symbols(&table);
        symbols.SetHalfWidthSymbols(true);
        symbols.SetPunctuationStyle(PunctuationStyle::CommaPeriod);
        Type(symbols, L"(q)!,");
        RequireText(symbols.Commit(), L"(\xFF51)!\xFF0C", "half-width symbols; the comma key keeps its style");
        symbols.SetHalfWidthLetters(true);
        Type(symbols, L"(q)!,");
        RequireText(symbols.Commit(), L"(q)!\xFF0C", "half-width letters are a setting of their own");
        symbols.SetHalfWidthSymbols(false);
        Type(symbols, L"q");
        symbols.Convert();
        const auto* widths = symbols.FocusedCandidates();
        Require(widths && widths->size() > 1 && widths->front().text == L"q" && (*widths)[1].text == L"\xFF51",
                "a letter converts to itself, then to the other width");
        symbols.Clear();
        symbols.SetHalfWidthLetters(false);
        Type(symbols, L"q");
        symbols.Convert();
        widths = symbols.FocusedCandidates();
        Require(widths && widths->size() > 1 && widths->front().text == L"\xFF51" && (*widths)[1].text == L"q",
                "full-width first when letters are full-width");
        symbols.Clear();
        const auto has = [&](const std::wstring& text) {
            const auto* list = symbols.FocusedCandidates();
            return list && std::any_of(list->begin(), list->end(), [&](const auto& c) { return c.text == text; });
        };
        Type(symbols, L"!");
        symbols.Convert();
        Require(has(L"\xFF01") && has(L"!"), "a symbol alone converts to both widths");
        symbols.Clear();
        Type(symbols, L"/");
        symbols.Convert();
        Require(has(L"/"), "a Japanese mark alone converts to the key it was typed with");
        symbols.Clear();
        RequireText(Committed(table, L"!?"), L"\xFF01\xFF1F", "full-width symbols by default");
    }
    {
        auto numbered = converter;
        const auto parts = tekito::japanese::UserPartsOfSpeech::Load(std::filesystem::path(TEKITO_TEST_DATA_DIR) /
                                                                     L"japanese-mini" / L"pos.tsv");
        Require(parts.Number().has_value(), "the test pack has a part of speech for numbers");
        numbered.SetNumberWord(tekito::japanese::JapaneseConverter::NumberWord{parts.Number()->left, parts.Number()->right,
                                                                              parts.Number()->cost});
        const auto phrases = numbered.Convert(L"12345");
        Require(phrases.size() == 1 && phrases[0].candidates.front().text == L"12345", "digits are one number");
        JapaneseComposer half(&table);
        half.SetConverter(&numbered);
        half.SetSpecialConversions(&special);
        half.SetHalfWidthDigits(true);
        Type(half, L"2026");
        half.Convert();
        Require(half.Preedit() == L"2026" && (*half.FocusedCandidates())[1].text == L"\xFF12\xFF10\xFF12\xFF16" &&
                    std::any_of(half.FocusedCandidates()->begin(), half.FocusedCandidates()->end(),
                                [](const auto& c) { return c.text == L"二千二十六"; }),
                "half-width first, then full-width and kanji");
    }

    // After digits, a phrase is learned apart: こ learned as 子 on its own
    // leaves 3こ as 3個.
    {
        auto numbered = converter;
        const auto parts = tekito::japanese::UserPartsOfSpeech::Load(std::filesystem::path(TEKITO_TEST_DATA_DIR) /
                                                                     L"japanese-mini" / L"pos.tsv");
        numbered.SetNumberWord(tekito::japanese::JapaneseConverter::NumberWord{parts.Number()->left, parts.Number()->right,
                                                                              parts.Number()->cost});
        tekito::japanese::JapaneseLearningStore learning;
        for (int i = 0; i < 3; ++i) learning.RecordChoice(L"こ", L"子", L"子");
        JapaneseComposer counted(&table);
        counted.SetConverter(&numbered);
        counted.SetLearning(&learning);
        counted.SetHalfWidthDigits(true);
        Type(counted, L"3ko");
        counted.Convert();
        const auto withCounter = counted.Preedit();
        RequireText(withCounter, L"3個", "what こ alone was learned as does not follow a number");
        RequireText(counted.Commit(), L"3個", "3個 is committed");
        Require(learning.Preference(L"#こ", withCounter.substr(1)) > 0 && learning.Preference(L"こ", L"子") == 3,
                "the counter is learned apart from こ alone");
    }

    // Postal codes: the addresses they cover, from the japanese-zipcode pack.
    {
        using tekito::japanese::PostalCodeDigits;
        Require(PostalCodeDigits(L"1000001") == L"1000001" && PostalCodeDigits(L"\xFF11\xFF10\xFF10\x30FC\xFF10\xFF10\xFF10\xFF11") == L"1000001",
                "seven digits, with or without the hyphen, in either width");
        Require(!PostalCodeDigits(L"100001") && !PostalCodeDigits(L"1000-001") && !PostalCodeDigits(L"100a001"),
                "anything else is not a postal code");
        const auto directory = std::filesystem::temp_directory_path() / L"tekito-zipcode-test";
        std::filesystem::create_directories(directory);
        std::ofstream(directory / L"zipcodes.tsv", std::ios::binary)
            << "1000001\t" << Utf8(L"東京都千代田区千代田") << "\n1500001\t" << Utf8(L"東京都渋谷区神宮前") << "\n";
        auto postal = std::make_unique<tekito::japanese::PostalCodes>();
        Require(postal->Open(directory), "the postal codes open");
        RequireText(postal->Addresses(L"1500001", 9).at(0), L"東京都渋谷区神宮前", "an address by its code");
        JapaneseComposer typing(&table);
        typing.SetConverter(&converter);
        typing.SetSpecialConversions(&special);
        typing.SetPostalCodes(postal.get());
        typing.SetHalfWidthDigits(true);
        Type(typing, L"100-0001");
        typing.Convert();
        const auto* offered = typing.FocusedCandidates();
        Require(offered && offered->size() >= 3 && (*offered)[0].text == L"100-0001" &&
                    (*offered)[1].text == L"東京都千代田区千代田" && (*offered)[2].text == L"\x3012" L"100-0001",
                "a postal code offers its address and its mark after it");
        typing.Clear();
        Type(typing, L"1234567");
        typing.Convert();
        Require(typing.FocusedCandidates() && std::none_of(typing.FocusedCandidates()->begin(), typing.FocusedCandidates()->end(),
                                                            [](const auto& c) { return c.text.starts_with(L"\x3012"); }),
                "seven digits no address covers are only a number");
        typing.Clear();
        typing.SetPostalCodes(nullptr);
        postal.reset();  // unmaps the file
        std::filesystem::remove_all(directory);
    }

    // A short input brings no other words as slips.
    {
        JapaneseComposer quiet(&table);
        quiet.SetConverter(&converter);
        Type(quiet, L"ko");
        quiet.Convert();
        quiet.Convert();
        const auto* list = quiet.FocusedCandidates();
        Require(list && std::none_of(list->begin(), list->end(), [](const auto& c) { return c.slip; }),
                "ko is not read as other keys");
    }

    tekito::SpecialConversionOptions off;
    off.calculator = off.numbers = off.symbols = off.dates = false;
    composer.SetSpecialConversions(&special, off);
    Type(composer, L"1+2=");
    composer.Convert();
    Require(composer.Preedit() != L"3", "with the calculator off, 1+2= is not summed");
    composer.Clear();
}

// The sentence check (japanese-reranker): pieces as SentencePiece reads
// them and ln p as the float model gives it (tests/data/ja/
// make-japanese-reranker-mini.py wrote both), the choice, and the composer.
void TestSentenceReranker(const RomajiTable& table, const MiniPack& pack) {
    tekito::japanese::SentenceReranker reranker;
    Require(!reranker.Open(std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"no-such-pack"), "a missing pack does not open");
    const auto root = std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-reranker-mini";
    Require(reranker.Open(root), "the test reranker opens");
    std::ifstream expected(root / L"expected.tsv", std::ios::binary);
    std::size_t rows = 0;
    for (std::string line; std::getline(expected, line); ++rows) {
        const std::size_t tab = line.find('\t'), second = line.find('\t', tab + 1);
        const std::wstring text = tekito::japanese::FromUtf8(std::string_view(line).substr(0, tab));
        std::vector<std::uint32_t> ids;
        std::istringstream pieces(line.substr(tab + 1, second - tab - 1));
        for (std::uint32_t id = 0; pieces >> id;) ids.push_back(id);
        Require(reranker.Pieces(text) == ids, ("the pieces of " + Utf8(text)).c_str());
        const std::wstring sentence[] = {text};
        const double logProbability = reranker.LogProbabilities(sentence).front();
        Require(std::abs(logProbability - std::stod(line.substr(second + 1))) < 0.1, ("ln p of " + Utf8(text)).c_str());
    }
    Require(rows == 8, "every expected row is read");

    // ln p: abc 12 -22.5, 今日は☃ -35.4; the prior: 0, -0.5, -1; no margin.
    const std::wstring worseFirst[] = {L"今日は☃", L"abc 12"};
    Require(reranker.Choose(worseFirst) == 1, "a sentence that reads much better is taken");
    const std::wstring betterFirst[] = {L"abc 12", L"今日は☃"};
    Require(reranker.Choose(betterFirst) == 0, "the first stays when it reads best");
    const std::wstring alone[] = {L"今日は☃"};
    Require(reranker.Choose(alone) == 0, "one sentence is kept");

    // The composer: the check only picks among the phrases' candidates, and
    // its switch turns it off.
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer plain(&table);
    plain.SetConverter(&converter);
    JapaneseComposer checked(&table);
    checked.SetConverter(&converter);
    checked.SetSentenceReranker(&reranker);
    for (const wchar_t* keys : {L"watashinonamaehanakanodesu", L"kikaigatomaru", L"kyouhaiitenkidesune"}) {
        Type(plain, keys);
        plain.Convert();
        Type(checked, keys);
        checked.Convert();
        const auto expectedSegments = plain.Segments();
        const auto segments = checked.Segments();
        Require(segments.size() == expectedSegments.size(), "the check keeps the phrases");
        Require(!checked.Commit().empty(), "the checked text commits");
        checked.SetSentenceCheck(false);
        Type(checked, keys);
        checked.Convert();
        RequireText(checked.Preedit(), plain.Preedit(), "with the check off, conversion is as without it");
        checked.Clear();
        checked.SetSentenceCheck(true);
        plain.Clear();
    }

    // What learning put first stays first.
    tekito::japanese::JapaneseLearningStore learning;
    checked.SetLearning(&learning);
    Type(checked, L"kikaigatomaru");
    checked.Convert();
    const auto* candidates = checked.FocusedCandidates();
    const auto first = checked.Segments().front().text;
    std::size_t other = 1;
    while (other < candidates->size() && (*candidates)[other].text == first) ++other;
    const auto chosen = (*candidates)[other].text;
    checked.SelectCandidate(other);
    (void)checked.Commit();
    Type(checked, L"kikaigatomaru");
    checked.Convert();
    RequireText(checked.Segments().front().text, chosen, "the check keeps what learning put first");
    checked.Clear();
}

// The slip finder (japanese-slips): ln p of kana by the n-grams and the
// LSTM as tests/data/ja/make-japanese-slips-mini.py worked them out, the
// readings it offers, and the composer going back to what was typed.
void TestKanaSlips(const RomajiTable& table, const MiniPack& pack) {
    tekito::japanese::KanaSlips slips;
    Require(!slips.Open(std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"no-such-pack"), "a missing slips pack does not open");
    const auto root = std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-slips-mini";
    Require(slips.Open(root), "the test slips pack opens");
    std::ifstream expected(root / L"expected.tsv", std::ios::binary);
    std::size_t rows = 0;
    for (std::string line; std::getline(expected, line); ++rows) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t tab = line.find('\t'), second = line.find('\t', tab + 1);
        const std::wstring kana = tekito::japanese::FromUtf8(std::string_view(line).substr(0, tab));
        const double ngram = std::stod(line.substr(tab + 1, second - tab - 1));
        const double neural = std::stod(line.substr(second + 1));
        Require(std::abs(slips.NgramLogProbability(kana) - ngram) < 0.01, ("n-gram ln p of " + Utf8(kana)).c_str());
        const std::wstring one[] = {kana};
        Require(std::abs(slips.NeuralLogProbabilities(one).front() - neural) < 0.1, ("LSTM ln p of " + Utf8(kana)).c_str());
    }
    Require(rows == 6, "every expected slips row is read");

    // The typed keys come first; each other reading is the typed keys with
    // its edits made.
    const std::wstring typed = L"kyouhaiitenkideune.";
    const auto found = slips.Find(typed, table, 5);
    Require(!found.empty() && found.front().keys == typed && found.front().edits.empty(), "the typed keys come first");
    for (std::size_t i = 1; i < found.size(); ++i) {
        std::wstring keys;
        std::size_t at = 0;
        for (const auto& edit : found[i].edits) {
            keys += typed.substr(at, edit.begin - at) + edit.with;
            at = edit.end;
        }
        keys += typed.substr(at);
        Require(keys == found[i].keys && keys != typed, "a reading is the typed keys with its edits");
        Require(found[i].edits.size() <= 2, "at most two slips are undone");
        Require(i == 1 || found[i].score <= found[i - 1].score, "readings come best first");
    }
    Require(slips.Find(std::wstring(400, L'a'), table, 5).empty(), "very long input is left alone");

    // Whatever it converts, Esc goes back to the kana as typed.
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetKanaSlips(&slips);
    Type(composer, typed);
    composer.Convert();
    Require(composer.IsConverted(), "keys with a slip convert");
    composer.Cancel();
    RequireText(composer.Preedit(), L"きょうはいいてんきでうね。", "Esc shows the kana as typed");
    composer.Clear();

    // Live: once typing pauses, what is shown is what Enter takes.
    Type(composer, typed);
    composer.Convert();
    const std::wstring converted = composer.Preedit();
    composer.Clear();
    composer.SetLiveConversion(true);
    Type(composer, typed);
    (void)composer.RefineLive();
    RequireText(composer.Preedit(), converted, "a pause shows the text as Space converts it");
    Require(!composer.RefineLive(), "a second pause changes nothing");
    RequireText(composer.Commit(), converted, "Enter takes what the pause showed");
    composer.SetLiveRefine(false);
    Type(composer, typed);
    Require(!composer.RefinesOnPause() && !composer.RefineLive(), "with the pause off, nothing is converted again");
    composer.Clear();
    composer.SetLiveRefine(true);
    // Off: a pause must not rewrite the reading via slips (same gate as Space).
    composer.SetSlipAutoApply(false);
    Type(composer, typed);
    const std::wstring liveAsTyped = composer.Preedit();
    (void)composer.RefineLive();
    RequireText(composer.Preedit(), liveAsTyped, "with slip auto-apply off, a pause keeps the typed conversion");
    composer.Clear();
    composer.SetSlipAutoApply(true);
    composer.SetLiveConversion(false);
}

// English typed with a slip among Japanese (Mix): put right as English
// Auto would, before a slip that makes Japanese of it, after English or
// with letters romaji hardly uses.
void TestMixEnglishSlips(const RomajiTable& table, const MiniPack& pack) {
    tekito::PreloadDefaultEngineData();
    const auto engine = tekito::CreateDefaultConversionEngine();
    Require(engine != nullptr, "English Auto loads for the Mix slip tests");
    const tekito::EnglishSpelling spelling(*engine);
    const tekito::ExternalFrequencyProvider scores(tekito::ExternalFrequencyProvider::DefaultPath());
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    const tekito::japanese::KeyConverter keys(pack.dictionary, pack.matrix, converter, table);
    const tekito::japanese::ChunkJudge judge(table, converter, &keys, scores, nullptr);
    const auto channel = judge.Judge(L"chaannel", 0, true, &spelling);
    Require(channel.english && channel.written == L"channel", "chaannel in Japanese is channel with a key typed twice");
    const auto review = judge.Judge(L"reviw", 1, true, &spelling, L"code ");
    Require(review.english && review.written == L"review", "after English, reviw is review");
}

// UTF-8 the same where wchar_t is 16 bits (Windows) or 32 (Linux).
void TestUtf8() {
    namespace utf8 = tekito::utf8;
    const std::string face = "\xF0\x9F\xAB\xA5";  // U+1FAE5, outside the BMP
    const auto decoded = utf8::Decode(face);
    Require(decoded && decoded->size() == (sizeof(wchar_t) == 2 ? 2u : 1u), "a character outside the BMP decodes");
    Require(utf8::Encode(*decoded) == face, "and encodes back to the same bytes");
    Require(utf8::Encode(L"かな") == "\xE3\x81\x8B\xE3\x81\xAA", "kana encode to UTF-8");
    Require(!utf8::Decode("\xC3") && !utf8::Decode("\xED\xA0\x80") && !utf8::Decode("\xC0\xAF"),
            "a cut-off sequence, a surrogate and an overlong form are not UTF-8");
    RequireText(utf8::DecodeLenient("a\xFF" "b"), L"a\xFFFD" L"b", "a bad byte is read as U+FFFD, the rest kept");
    std::vector<std::uint16_t> units;
    utf8::ForEachUtf16(*decoded, [&](std::uint16_t unit) { units.push_back(unit); });
    Require(units == std::vector<std::uint16_t>{0xD83E, 0xDEE5}, "it is two UTF-16 units however wchar_t is");
    // The language model hashes UTF-16 units, as its builder does.
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const std::uint16_t unit : units) {
        hash = (hash ^ (unit & 0xFF)) * 0x100000001B3ULL;
        hash = (hash ^ (unit >> 8)) * 0x100000001B3ULL;
    }
    Require(tekito::japanese::LanguageModel::Hash(*decoded) == hash, "the model's hash is over UTF-16 units");
}

// Where candidates go in the list: the other language right after the
// first, a slip undone after it, and katakana words typed with a vowel for
// ー near the top.
void TestCandidateOrder(const RomajiTable& table, const MiniPack& pack) {
    {
        // What the sentence check puts sentences together by: each choice's
        // cost over the first, rising along the list; kana and the like,
        // without a cost of their own, a fixed amount over the first.
        using tekito::japanese::PhraseCandidate;
        std::vector<PhraseCandidate> list{{L"機械", 5000, PhraseCandidate::Kind::Dictionary, false},
                                          {L"機会", 5200, PhraseCandidate::Kind::Dictionary, false},
                                          {L"奇怪", 5100, PhraseCandidate::Kind::Dictionary, false},
                                          {L"きかい", 0, PhraseCandidate::Kind::Hiragana, false},
                                          {L"器械", 9000, PhraseCandidate::Kind::Dictionary, false}};
        const auto deltas = JapaneseComposer::CostDeltas(list, {0, 1, 2, 3, 4});
        Require(deltas.size() == 5 && deltas[0] == 0 && deltas[1] == 200, "a choice costs what it costs over the first");
        Require(deltas[2] == 200, "and never less than the one before it (the list's order kept)");
        Require(deltas[3] == 3000 && deltas[4] == 4000, "kana take a fixed amount; later words their own cost");
        Require(JapaneseComposer::CostDeltas(list, {1, 0}) == std::vector<std::int64_t>{0, 0},
                "the first of the list given is the one counted from");
    }
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    {
        // nyuusu: ニュース, read as にゅーす; the particle after it kept.
        JapaneseComposer composer(&table);
        composer.SetConverter(&converter);
        Type(composer, L"nyuusuwo");
        composer.Convert();
        bool found = false;
        for (std::size_t f = 0; f < composer.Segments().size() && !found; ++f) {
            composer.MoveFocus(-static_cast<int>(composer.Segments().size()));
            composer.MoveFocus(static_cast<int>(f));
            const auto* list = composer.FocusedCandidates();
            for (std::size_t c = 0; list && c < std::min<std::size_t>(3, list->size()); ++c) {
                found = found || (*list)[c].text.starts_with(L"ニュース");
            }
        }
        Require(found, "a katakana word typed with a vowel for its long mark is among the first candidates");
        composer.Clear();
    }

    struct WordScores final : tekito::IFrequencyProvider {
        double Score(std::wstring_view word) const noexcept override {
            for (const auto& [listed, score] : {std::pair<std::wstring_view, double>{L"sake", 4.0}, {L"push", 4.2}}) {
                if (word == listed) return score;
            }
            return 0.0;
        }
    } scores;
    const tekito::japanese::ChunkJudge judge(table, converter, nullptr, scores, nullptr);
    const auto sake = judge.Judge(L"sake", 0);
    Require(!sake.english && sake.either, "sake reads both ways and is taken as Japanese");
    const tekito::japanese::KeyConverter keys(pack.dictionary, pack.matrix, converter, table);
    {
        JapaneseComposer composer(&table);
        composer.SetConverter(&converter);
        composer.SetKeyConverter(&keys);
        composer.SetMixedTyping(&judge);
        Type(composer, L"sake");
        composer.Convert();
        composer.ShowCandidates();
        const auto* list = composer.FocusedCandidates();
        Require(list && list->size() >= 2 && (*list)[1].otherLanguage &&
                    (*list)[1].kind == tekito::japanese::PhraseCandidate::Kind::English,
                "the English of a chunk that reads both ways is second, also with the list open");
        // Where a slip undone goes (JapaneseComposer::SlipPosition).
        using tekito::japanese::PhraseCandidate;
        const auto make = [](std::initializer_list<std::pair<const wchar_t*, PhraseCandidate::Kind>> items) {
            std::vector<PhraseCandidate> out;
            for (const auto& [text, kind] : items) out.push_back({text, 0, kind, false});
            return out;
        };
        constexpr auto kDict = PhraseCandidate::Kind::Dictionary;
        constexpr auto kEnglish = PhraseCandidate::Kind::English;
        auto plain = make({{L"立た", kDict}, {L"他", kDict}, {L"多", kDict}, {L"田", kDict}, {L"打", kDict}, {L"駄", kDict}});
        Require(JapaneseComposer::SlipPosition(plain, true) == 1, "a likelier slip goes right after the first");
        Require(JapaneseComposer::SlipPosition(plain, false) > 1, "another waits further down");
        auto either = plain;
        either.insert(either.begin() + 1, PhraseCandidate{L"Data", 0, kEnglish, false});
        either[1].otherLanguage = true;
        Require(JapaneseComposer::SlipPosition(either, true) > 1,
                "a slip does not go before the chunk in the other language");
        auto english = make({{L"Online", kEnglish}, {L"online", kEnglish}, {L"オンライン", kDict}, {L"x", kDict},
                             {L"y", kDict}, {L"z", kDict}});
        Require(JapaneseComposer::SlipPosition(english, true) > 1, "nor second in a chunk taken as English");
        Require(JapaneseComposer::SlipPosition({}, true) == 0, "an empty list takes it first");
        // Keys that spell a word in both languages are no likely slip:
        // their own words come first (避け, Sake, 酒, 鮭, then かけ).
        for (std::size_t c = 1; c < std::min<std::size_t>(4, list->size()); ++c) {
            Require(!(*list)[c].slip, "a slip undone waits after a chunk's own words when it reads both ways");
        }
        composer.Clear();

        // Japanese chunks are converted again as one on Enter, so a word
        // reads on from those after it: sake wo nomu is 酒を飲む, as
        // sakewonomu would be, whatever sake alone converts to.
        composer.ForgetContext();
        for (const auto* chunk : {L"sake", L"wo", L"nomu"}) {
            Type(composer, chunk);
            composer.Convert();
        }
        JapaneseComposer whole(&table);
        whole.SetConverter(&converter);
        Type(whole, L"sakewonomu");
        whole.Convert();
        const std::wstring together = whole.Commit();
        RequireText(composer.Commit(), together, "Japanese chunks read as one sentence on Enter");
        composer.ForgetContext();

        Type(composer, L"push");
        composer.Convert();
        composer.ShowCandidates();
        list = composer.FocusedCandidates();
        Require(list && !list->empty() && list->front().kind == tekito::japanese::PhraseCandidate::Kind::English,
                "push is English");
        Require(list->size() < 2 || !(*list)[1].slip, "a chunk taken as English has no slip of Japanese second");
        composer.Clear();
    }
}

int main(int argc, char** argv) {
    const auto table = LoadTable();
    TestKanaText();
    TestRomajiTable(*table);
    TestRomajiChanges();
    TestRomajiConversion(*table);
    TestEditing(*table);
    TestCaret(*table);
    TestConversionForms(*table);
    TestInputFormAndPunctuation(*table);
    const auto pack = LoadMiniPack();
    TestDictionary(*pack);
    TestAddedWords(*pack);
    TestDamagedPacks();
    TestConversion(*pack);
    TestComposerConversion(*table, *pack);
    TestKeys(*table, *pack);
    TestLiveConversion(*table, *pack);
    TestCapitals(*table, *pack);
    TestUndoCommit(*table, *pack);
    TestReconvert(*table, *pack);
    TestLearningStore();
    TestComposerLearning(*table, *pack);
    TestContext(*table, *pack);
    TestLanguageModel(*pack);
    TestLoanwords(*table, *pack);
    TestMixedTyping(*table, *pack);
    TestRomajiCorrection(*table, *pack);
    TestPrediction(*table, *pack);
    TestUserWords(*table, *pack);
    TestSpecialConversions(*table, *pack);
    TestSentenceReranker(*table, *pack);
    TestKanaSlips(*table, *pack);
    TestCandidateOrder(*table, *pack);
    TestMixEnglishSlips(*table, *pack);
    TestUtf8();
    TestKanjiSets();
    TestMeanings();
    if (argc > 1 && std::string_view(argv[1]) == "--dump") DumpConversions(*pack);
    std::cout << "All TEKITO Japanese tests passed.\n";
    return 0;
}
