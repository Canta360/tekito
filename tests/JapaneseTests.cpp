#include "Core/ExternalLexiconProvider.h"
#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseConverter.h"
#include "Core/Japanese/JapaneseDictionary.h"
#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/JapaneseUserDictionary.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/LanguageModel.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/Meanings.h"
#include "Core/Japanese/RomajiTable.h"
#include "Core/SpecialConversions.h"

#include <algorithm>
#include <cmath>
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

void TestLoanwords(const RomajiTable& table, const MiniPack& pack) {
    using tekito::japanese::LoanwordKey;
    RequireText(LoanwordKey(L"ミーティング"), L"みいちんぐ", "long vowels are written out, ティ is チ");
    RequireText(LoanwordKey(L"ファイル"), L"ふあいる", "small vowels are full size");

    tekito::japanese::Loanwords loanwords;
    Require(loanwords.Open(tekito::ExternalLexiconProvider::DataPackRoot() / L"japanese-loanwords"),
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

    // Slips that still read as kana: a neighboring key, a key dropped, an
    // extra key, two keys swapped.
    for (const auto* keys : {L"arigayougozaimasu", L"arigtougozaimasu", L"arigatouggozaimasu",
                             L"arigatougozaimsau"}) {
        RequireText(Type(composer, keys).substr(0, 2), L"あり", "the slip is typed as it is");
        composer.Convert();
        RequireText(composer.Preedit(), L"ありがとうございます", "Space converts what was meant");
        composer.Clear();
    }
    Type(composer, L"shigotogaowatta");
    composer.Convert();
    const std::wstring meant = composer.Preedit();
    composer.Clear();
    Type(composer, L"shigotogaowqtta");
    composer.Convert();
    RequireText(composer.Preedit(), meant, "a slip inside a sentence");
    composer.Clear();

    // A slip the first choice keeps (a dropped consonant reads as other
    // kana) is one pick away once the list opens, marked as a slip.
    Type(composer, L"ikaigatomaru");
    composer.Convert();
    const std::wstring first = composer.Preedit();
    composer.NextCandidate();
    const auto* listed = composer.FocusedCandidates();
    Require(first.find(L"機械") == std::wstring::npos, "the first choice is what was typed");
    Require(std::any_of(listed->begin(), listed->end(),
                        [](const tekito::japanese::PhraseCandidate& c) {
                            return c.slip && c.text.starts_with(L"機械が");
                        }),
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
    composer.SelectCandidate(static_cast<std::size_t>(whole - phraseList->begin()));
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
    // The builder's hashes and these meet (scripts/build-japanese-lm.py --mini).
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
    composer.Cancel();
    composer.Clear();

    composer.SetLearning(nullptr);
    Type(composer, L"kikaigatomaru");
    composer.Convert();
    RequireText(composer.Segments().front().text, first, "without learning the order is the converter's");
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
    Require(special.Load(tekito::ExternalLexiconProvider::DataPackRoot() / L"special-conversions" / L"rules.tsv"),
            "the special-conversions pack loads");

    const LocalTime morning{2026, 9, 29, 6, 5};  // a Tuesday
    const auto today = special.Dates(Language::English, L"today", morning);
    Require(today.size() >= 3, "today has several formats");
    RequireText(today[0], L"September 29, 2026", "today, written out");
    Require(Contains(today, L"2026-09-29") && Contains(today, L"Tuesday, September 29"), "today as ISO and with the weekday");
    Require(Contains(special.Dates(Language::English, L"now", morning), L"6:05 AM"), "now, in the morning");
    RequireText(special.Dates(Language::English, L"tomorrow", morning)[0], L"September 30, 2026", "tomorrow");
    Require(special.Dates(Language::English, L"table", morning).empty(), "other words have no date");

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
    Require(special.Numbers(L"0120").size() == 2, "a number that starts with 0 only changes width");
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

    tekito::SpecialConversionOptions off;
    off.calculator = off.numbers = off.symbols = off.dates = false;
    composer.SetSpecialConversions(&special, off);
    Type(composer, L"1+2=");
    composer.Convert();
    Require(composer.Preedit() != L"3", "with the calculator off, 1+2= is not summed");
    composer.Clear();
}

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
    TestContext(*table, *pack);
    TestLanguageModel(*pack);
    TestLoanwords(*table, *pack);
    TestRomajiCorrection(*table, *pack);
    TestPrediction(*table, *pack);
    TestUserWords(*table, *pack);
    TestSpecialConversions(*table, *pack);
    TestMeanings();
    if (argc > 1 && std::string_view(argv[1]) == "--dump") DumpConversions(*pack);
    std::cout << "All TEKITO Japanese tests passed.\n";
    return 0;
}
