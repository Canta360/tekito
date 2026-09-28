#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tekito::japanese {

// What a word the user added is, as Settings offers it. Each has a part of
// speech in the japanese-core pack's pos.tsv, keyed by KindName.
enum class UserWordKind : std::uint8_t {
    Noun,
    ProperNoun,
    Person,
    Surname,
    GivenName,
    Place,
    Organization,
    SuruNoun,  // a noun that takes する (勉強)
    Symbol,    // emoticons and marks
    Interjection,
};

[[nodiscard]] std::wstring_view KindName(UserWordKind kind) noexcept;
[[nodiscard]] std::optional<UserWordKind> KindFromName(std::wstring_view name) noexcept;

// A word the user added: its hiragana reading and how it is written.
struct UserWord {
    std::wstring reading;
    std::wstring surface;
    UserWordKind kind{UserWordKind::Noun};

    bool operator==(const UserWord&) const = default;
};

// The part of speech (the matrix's ids) and cost each kind takes.
class UserPartsOfSpeech final {
public:
    struct Entry {
        std::uint16_t left{0};
        std::uint16_t right{0};
        std::int32_t cost{0};
    };

    // japanese-core/pos.tsv: kind, left id, right id, cost. Empty if the
    // pack has none (packs built before it).
    [[nodiscard]] static UserPartsOfSpeech Load(const std::filesystem::path& file);
    [[nodiscard]] static UserPartsOfSpeech Parse(std::string_view text);
    [[nodiscard]] std::optional<Entry> For(UserWordKind kind) const noexcept;
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }

private:
    std::map<UserWordKind, Entry> entries_;
};

// The user's words, ready for conversion: looked up by reading.
class JapaneseUserDictionary final {
public:
    struct Entry {
        std::wstring reading;
        std::wstring surface;
        std::uint16_t left{0};
        std::uint16_t right{0};
        std::int32_t cost{0};
    };

    // Words whose kind has no part of speech are left out.
    void Set(const std::vector<UserWord>& words, const UserPartsOfSpeech& parts);
    [[nodiscard]] bool Empty() const noexcept { return byReading_.empty(); }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

    // The words read exactly `reading`.
    [[nodiscard]] const std::vector<Entry>* Exact(std::wstring_view reading) const;
    // Each word whose reading begins `text`: found(length, entry).
    template <typename Found>
    void ForEachPrefixOf(std::wstring_view text, Found&& found) const {
        const std::size_t longest = std::min(longest_, text.size());
        for (std::size_t length = 1; length <= longest; ++length) {
            if (const auto* entries = Exact(text.substr(0, length))) {
                for (const auto& entry : *entries) found(length, entry);
            }
        }
    }
    // The words whose reading is longer than `prefix` and starts with it.
    [[nodiscard]] std::vector<const Entry*> StartingWith(std::wstring_view prefix) const;

private:
    std::map<std::wstring, std::vector<Entry>, std::less<>> byReading_;
    std::size_t longest_{0};
    std::size_t size_{0};
};

}  // namespace tekito::japanese
