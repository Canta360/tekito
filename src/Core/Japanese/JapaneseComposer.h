#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

class RomajiTable;

// How the whole composition is written.
enum class KanaForm : std::uint8_t {
    Hiragana,
    Katakana,
    HalfWidthKatakana,
    FullWidthAlphanumeric,  // the keys as typed, full-width (F9)
    HalfWidthAlphanumeric,  // the keys as typed (F10)
};

// Which marks the 、 and 。 keys write (UserSettings::japanesePunctuation).
enum class PunctuationStyle : std::uint8_t {
    ToutenKuten,   // 、。
    CommaPeriod,   // ，．
    CommaKuten,    // ，。
    ToutenPeriod,  // 、．
};

// The text being typed in Japanese mode, before it is committed.
//
// Keys go through the romaji table as they are typed; keys that may still
// become kana ("k", "n", "ky") stay pending. Space converts the whole text
// (for now only between hiragana and katakana; kana-kanji conversion comes
// with the dictionary), F6-F10 and Muhenkan write it in another form, and
// after any of those the next key the caller sees commits the text, as in
// Microsoft IME.
class JapaneseComposer final {
public:
    explicit JapaneseComposer(const RomajiTable* table = nullptr) noexcept : table_(table) {}

    void SetTable(const RomajiTable* table) noexcept;
    void SetPunctuationStyle(PunctuationStyle style) noexcept { punctuation_ = style; }
    // Hiragana or Katakana: how new text is shown while typing.
    void SetInputForm(KanaForm form) noexcept;
    [[nodiscard]] KanaForm InputForm() const noexcept { return inputForm_; }

    [[nodiscard]] bool IsComposing() const noexcept { return !units_.empty() || !pending_.empty(); }
    // True after Space, Muhenkan or F6-F10: typing on commits the text.
    [[nodiscard]] bool IsConverted() const noexcept { return converted_; }
    [[nodiscard]] KanaForm Form() const noexcept { return form_; }

    void Insert(wchar_t key);
    void Backspace();
    // Back to the typed kana after a conversion; otherwise drops everything.
    void Cancel();
    // Space and Henkan.
    void Convert();
    // Muhenkan: hiragana -> katakana -> half-width katakana -> hiragana.
    void CycleKana();
    // F6-F10. Pressing F9 or F10 again changes the letter case.
    void Transliterate(KanaForm form);

    // What to show in the document while composing.
    [[nodiscard]] std::wstring Preedit() const;
    // The text to commit (pending keys are resolved first); clears the
    // composition.
    [[nodiscard]] std::wstring Commit();
    void Clear() noexcept;

private:
    struct Unit {
        std::wstring keys;  // what was typed
        std::wstring kana;  // what the table wrote for it
    };

    void Feed(wchar_t key);
    void FlushOnce();
    void FlushAll();
    void Emit(std::wstring keys, std::wstring kana);
    [[nodiscard]] std::wstring Render(bool includePending) const;
    [[nodiscard]] std::wstring Keys(bool includePending) const;
    [[nodiscard]] std::wstring ApplyPunctuation(std::wstring text) const;

    const RomajiTable* table_{nullptr};
    std::vector<Unit> units_;
    std::wstring pending_;
    KanaForm inputForm_{KanaForm::Hiragana};
    KanaForm form_{KanaForm::Hiragana};
    PunctuationStyle punctuation_{PunctuationStyle::ToutenKuten};
    bool converted_{false};
    // F9/F10 pressed again: 0 as typed, 1 upper case, 2 capitalized,
    // 3 lower case.
    int letterCase_{0};
};

}  // namespace tekito::japanese
