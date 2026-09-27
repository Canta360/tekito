#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/RomajiTable.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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
    return std::string(text.begin(), text.end());
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
    auto table = RomajiTable::Load(tekito::ExternalLexiconProvider::DataPackRoot() / L"japanese-romaji");
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

void TestConversionForms(const RomajiTable& table) {
    JapaneseComposer composer(&table);
    Type(composer, L"katakana");
    composer.Convert();
    Require(composer.IsConverted(), "Space converts");
    RequireText(composer.Preedit(), L"カタカナ", "Space gives katakana before the dictionary");
    composer.Convert();
    RequireText(composer.Preedit(), L"かたかな", "Space again goes back to hiragana");
    composer.Cancel();
    Require(!composer.IsConverted() && composer.IsComposing(), "Esc returns to the typed kana");
    RequireText(composer.Preedit(), L"かたかな", "the typed kana is kept");

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
    RequireText(composer.Commit(), L"Ｔエキト", "F7: katakana, the capital stays a letter");
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

}  // namespace

int main() {
    const auto table = LoadTable();
    TestKanaText();
    TestRomajiTable(*table);
    TestRomajiConversion(*table);
    TestEditing(*table);
    TestConversionForms(*table);
    TestInputFormAndPunctuation(*table);
    std::cout << "All TEKITO Japanese tests passed.\n";
    return 0;
}
