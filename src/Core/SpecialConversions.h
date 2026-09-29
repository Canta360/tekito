#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito {

// A moment in local time, to the minute.
struct LocalTime {
    int year{2000};
    int month{1};  // 1-12
    int day{1};
    int hour{0};
    int minute{0};

    [[nodiscard]] static LocalTime Now();
};

// Which of the special conversions are on (UserSettings).
struct SpecialConversionOptions {
    bool dates{true};
    bool numbers{true};
    bool symbols{true};
    bool calculator{true};
};

// The special-conversions pack (rules.tsv): what TEKITO offers besides
// words -- today's date for "today" and "きょう", symbols by their reading
// ("やじるし" -> →) or their ASCII spelling ("->" -> →), a number in kanji --
// and the arithmetic for "1+2=". Every word, format and name comes from the
// pack; without it, only the arithmetic and the plain number forms work.
class SpecialConversions final {
public:
    enum class Language : std::uint8_t { English, Japanese };

    // The installed pack, loaded once per process on first use (empty when
    // it is not installed).
    [[nodiscard]] static const SpecialConversions& Installed();

    bool Load(const std::filesystem::path& file);
    // rules.tsv as UTF-8: section, language, key, value on each row.
    void Parse(std::string_view text);
    [[nodiscard]] bool Empty() const noexcept { return rows_.empty(); }

    // What a date or time word is at `now`, in each of the language's
    // formats ("today" -> "September 29, 2026", ...); empty for other words.
    [[nodiscard]] std::vector<std::wstring> Dates(Language language, std::wstring_view word,
                                                  const LocalTime& now) const;
    // The symbols for a reading or an ASCII spelling, in the pack's order.
    // Japanese keys are matched in full-width, as they are typed there.
    [[nodiscard]] std::vector<std::wstring> Symbols(Language language, std::wstring_view key) const;
    // Japanese emoticons (kaomoji) for a reading ("にこにこ" -> (^^)); "かおもじ"
    // has them all.
    [[nodiscard]] std::vector<std::wstring> Emoticons(std::wstring_view reading) const;
    // Single kanji read this way ("こ" -> 己, 子, 小, ...), one per string.
    [[nodiscard]] std::vector<std::wstring> SingleKanji(std::wstring_view reading) const;
    // `digits` (half- or full-width) in its other forms: half- and
    // full-width, with commas, in kanji (千二百三十四, 一二三四, 壱阡弐百参拾四),
    // as a Roman numeral (up to 3999) and circled (up to 50). Empty unless
    // `digits` is all digits.
    [[nodiscard]] std::vector<std::wstring> Numbers(std::wstring_view digits) const;

    // What English text before the caret ends with, typed outside words: a
    // symbol's ASCII spelling ("->", "(c)") or a sum ("1+2=", "3 * 4 ="),
    // as far as `options` has them on. `length` is how much of the text it
    // takes; `offers` what it can become ("3" and "1+2=3" for "1+2=").
    struct Ending {
        std::size_t length{0};
        std::vector<std::wstring> offers;
    };
    [[nodiscard]] std::optional<Ending> EnglishEnding(std::wstring_view text, SpecialConversionOptions options) const;
    // Whether a key typing `c` may finish such an ending, so the text before
    // the caret is worth a look.
    [[nodiscard]] bool MayEndEnglish(wchar_t c, SpecialConversionOptions options) const noexcept;

    // The value of `expression` when it is arithmetic that ends in "=" ("1+2=",
    // "(3+4)*2=", "10/4="; half- or full-width, with × ÷ and, as Japanese
    // input types them, ー for minus and ・ for divide). Nothing for anything
    // else, or for dividing by zero.
    [[nodiscard]] static std::optional<std::wstring> Calculate(std::wstring_view expression);

private:
    [[nodiscard]] const std::vector<std::wstring>* Find(std::wstring_view section, Language language,
                                                        std::wstring_view key) const;
    [[nodiscard]] std::wstring Name(Language language, std::wstring_view name, std::size_t index) const;
    [[nodiscard]] std::wstring Kanji(std::wstring_view digits, bool daiji) const;
    [[nodiscard]] std::wstring Format(Language language, std::wstring_view pattern, int year, int month, int day,
                                      const LocalTime& now) const;

    // section \t language \t key -> values, in the pack's order.
    std::unordered_map<std::wstring, std::vector<std::wstring>> rows_;
    // The English symbol spellings, and the characters they end with.
    std::vector<std::wstring> englishSymbolKeys_;
    std::wstring englishSymbolEnds_;
    struct Era {
        int start{0};  // yyyymmdd
        std::wstring name;
    };
    std::vector<Era> eras_;  // latest first
};

}  // namespace tekito
