#pragma once

#include <filesystem>
#include <string_view>
#include <unordered_map>

namespace tekito::japanese {

// Which kanji candidates may use (UserSettings::japaneseKanjiRange).
enum class KanjiRange {
    All = 0,
    // 常用漢字 and 人名用漢字.
    JoyoAndJinmei = 1,
    Joyo = 2,
    // 教育漢字.
    Kyoiku = 3,
};

// 教育漢字, 常用漢字 and 人名用漢字, from the kanji-sets pack.
class KanjiSets final {
public:
    // kanji.tsv: "kanji<TAB>level", 1-6 = 教育 of that school year,
    // 8 = 常用, 9 = 人名用.
    bool Load(const std::filesystem::path& file);
    void Add(char32_t kanji, int level) { levels_[kanji] = level; }
    [[nodiscard]] bool Empty() const noexcept { return levels_.empty(); }

    // Every kanji in `text` is in `range` (kana, letters and marks always
    // are); for 教育, taught by school year `grade` (1-6).
    [[nodiscard]] bool Allows(std::wstring_view text, KanjiRange range, int grade = 6) const;
    // Every kanji in `text` is 常用 or 人名用.
    [[nodiscard]] bool Common(std::wstring_view text) const { return Allows(text, KanjiRange::JoyoAndJinmei); }

private:
    std::unordered_map<char32_t, int> levels_;
};

}  // namespace tekito::japanese
