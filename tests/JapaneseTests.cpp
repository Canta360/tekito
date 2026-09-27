#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/RomajiTable.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

struct MiniPack {
    tekito::japanese::JapaneseDictionary dictionary;
    tekito::japanese::ConnectionMatrix matrix;
};

std::unique_ptr<MiniPack> LoadMiniPack() {
    auto pack = std::make_unique<MiniPack>();
    const std::filesystem::path root = std::filesystem::path(TEKITO_TEST_DATA_DIR) / L"japanese-mini";
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

void TestEnglishInJapanese(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    std::wstring asked;
    composer.SetEnglishCandidates([&](std::wstring_view keys) {
        asked = keys;
        return std::vector<std::wstring>{L"thanks", L"thank"};
    });

    Type(composer, L"thnaks");
    composer.Convert();
    RequireText(asked, L"thnaks", "the English engine gets the keys as typed");
    RequireText(SegmentsText(composer), L"*thanks", "keys that are not romaji convert to English first");
    const auto* candidates = composer.FocusedCandidates();
    Require(candidates && candidates->size() >= 4 && (*candidates)[2].text == L"thnaks",
            "the keys as typed follow the English words");
    Require(candidates->back().kind == tekito::japanese::PhraseCandidate::Kind::Hiragana ||
                candidates->back().kind == tekito::japanese::PhraseCandidate::Kind::Katakana,
            "kana stay available");
    composer.Clear();

    Type(composer, L"kikai");
    composer.Convert();
    const auto* kikai = composer.FocusedCandidates();
    Require(kikai && kikai->front().kind == tekito::japanese::PhraseCandidate::Kind::Dictionary,
            "romaji that makes Japanese converts to Japanese first");
    Require(std::any_of(kikai->begin(), kikai->end(),
                        [](const auto& c) { return c.kind == tekito::japanese::PhraseCandidate::Kind::English; }),
            "English words come after, for a single phrase");
    composer.Clear();

    asked.clear();
    Type(composer, L"kikaigatomaru");
    composer.Convert();
    Require(composer.Segments().size() > 1 && composer.FocusedCandidates()->back().kind !=
                                                  tekito::japanese::PhraseCandidate::Kind::English,
            "a sentence of several phrases gets no English words");
}

void TestRomajiCorrection(const RomajiTable& table, const MiniPack& pack) {
    const tekito::japanese::JapaneseConverter converter(pack.dictionary, pack.matrix);
    JapaneseComposer composer(&table);
    composer.SetConverter(&converter);
    composer.SetEnglishCandidates([](std::wstring_view) { return std::vector<std::wstring>{L"english"}; });

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

    Type(composer, L"thnaks");
    composer.Convert();
    RequireText(composer.Preedit(), L"english", "many unreadable letters are English, not a slip");
    composer.Clear();

    Type(composer, L"kikai");
    composer.Convert();
    Require(composer.Preedit().find(L"機") != std::wstring::npos, "correct romaji is not touched");
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
    composer.Cancel();
    composer.Clear();

    composer.SetLearning(nullptr);
    Type(composer, L"kikaigatomaru");
    composer.Convert();
    RequireText(composer.Segments().front().text, first, "without learning the order is the converter's");
}

}  // namespace

int main(int argc, char** argv) {
    const auto table = LoadTable();
    TestKanaText();
    TestRomajiTable(*table);
    TestRomajiConversion(*table);
    TestEditing(*table);
    TestCaret(*table);
    TestConversionForms(*table);
    TestInputFormAndPunctuation(*table);
    const auto pack = LoadMiniPack();
    TestDictionary(*pack);
    TestDamagedPacks();
    TestConversion(*pack);
    TestComposerConversion(*table, *pack);
    TestLearningStore();
    TestComposerLearning(*table, *pack);
    TestEnglishInJapanese(*table, *pack);
    TestRomajiCorrection(*table, *pack);
    TestPrediction(*table, *pack);
    if (argc > 1 && std::string_view(argv[1]) == "--dump") DumpConversions(*pack);
    std::cout << "All TEKITO Japanese tests passed.\n";
    return 0;
}
