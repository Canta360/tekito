#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito::japanese {

// The japanese-notation pack (scripts/ja/build-japanese-notation.py): words
// of the dictionary written in kanji or in kana by level, 1 (more kanji) to
// 5 (more kana); 3 writes as 公用文における漢字使用等について does (下さい:
// ください, 事: こと, 又: また). A word marked for a level costs more where
// the other form is the one wanted there.
class Notation final {
public:
    static constexpr int kLowest = 1;
    static constexpr int kHighest = 5;
    static constexpr int kDefault = 3;

    bool Open(const std::filesystem::path& packDirectory);
    [[nodiscard]] bool IsOpen() const noexcept { return !words_.empty(); }

    // Whether any word of this part of speech (left id) is in the pack: a
    // quick check before Cost.
    [[nodiscard]] bool Has(std::uint16_t left) const noexcept { return left < parts_.size() && parts_[left]; }

    // What the word costs over its own cost at `level`: `cost` for its kanji
    // form at or above its level, and for its kana form below it when the
    // level asks for more kanji (1 and 2); 0 otherwise.
    [[nodiscard]] std::int64_t Cost(std::uint16_t left, std::wstring_view word, int level, std::int64_t cost) const;

private:
    // Key: the part of speech as one character, then the word. Value: the
    // level, plus 8 for the kanji form.
    std::unordered_map<std::wstring, std::uint8_t> words_;
    std::vector<bool> parts_;
};

}  // namespace tekito::japanese
