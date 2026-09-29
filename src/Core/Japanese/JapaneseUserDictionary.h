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

// What conversion does with a word the user added, as the English user
// dictionary's Replace / Suggest / Keep as typed.
enum class UserWordAction : std::uint8_t {
    First,     // converting its reading gives it first
    Suggest,   // offered in the list, second; the conversion is not changed
    Suppress,  // never offered for its reading (a wrong conversion to hide)
};

[[nodiscard]] std::wstring_view ActionName(UserWordAction action) noexcept;
[[nodiscard]] std::optional<UserWordAction> ActionFromName(std::wstring_view name) noexcept;

// A word the user added: its hiragana reading and how it is written.
struct UserWord {
    std::wstring reading;
    std::wstring surface;
    UserWordKind kind{UserWordKind::Noun};
    UserWordAction action{UserWordAction::First};

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
    // The row "number": digits typed in Japanese, read as one number word.
    [[nodiscard]] std::optional<Entry> Number() const noexcept { return number_; }

private:
    std::map<UserWordKind, Entry> entries_;
    std::optional<Entry> number_;
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

    // Words to offer whose kind has no part of speech are left out.
    void Set(const std::vector<UserWord>& words, const UserPartsOfSpeech& parts);
    [[nodiscard]] bool Empty() const noexcept {
        return byReading_.empty() && suggested_.empty() && suppressed_.empty();
    }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

    // The words to give first, read exactly `reading`.
    [[nodiscard]] const std::vector<Entry>* Exact(std::wstring_view reading) const;
    // The words only to offer, read exactly `reading`.
    [[nodiscard]] const std::vector<Entry>* Suggested(std::wstring_view reading) const;
    // Whether `surface` is never to be offered for `reading`.
    [[nodiscard]] bool HasSuppressed() const noexcept { return !suppressed_.empty(); }
    [[nodiscard]] bool Suppresses(std::wstring_view reading, std::wstring_view surface) const;
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
    // The words whose reading is longer than `prefix` and starts with it:
    // the ones to give first, or with `suggested` the ones only to offer.
    [[nodiscard]] std::vector<const Entry*> StartingWith(std::wstring_view prefix, bool suggested = false) const;

private:
    std::map<std::wstring, std::vector<Entry>, std::less<>> byReading_;
    std::map<std::wstring, std::vector<Entry>, std::less<>> suggested_;
    std::map<std::wstring, std::vector<std::wstring>, std::less<>> suppressed_;
    std::size_t longest_{0};
    std::size_t size_{0};
};

}  // namespace tekito::japanese
