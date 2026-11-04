#pragma once

#include "Core/Japanese/SortedTsv.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace tekito::japanese {

// The japanese-compounds pack (scripts/ja/build-japanese-compounds.py): how
// many of the dictionary's compounds each kanji, read one way, ends (筒
// read とう: 燃焼筒, 排気筒) and starts (再 read さい: 再利用), so the
// converter can put together compounds the dictionary does not have.
class Compounds final {
public:
    bool Open(const std::filesystem::path& packDirectory) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept { return table_.IsOpen(); }

    struct Counts {
        std::uint32_t ends{0};
        std::uint32_t starts{0};
    };
    [[nodiscard]] Counts Find(std::wstring_view kanji, std::wstring_view reading) const;

private:
    SortedTsv table_;
};

}  // namespace tekito::japanese
