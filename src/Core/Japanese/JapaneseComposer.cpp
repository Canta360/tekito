#include "Core/Japanese/JapaneseComposer.h"

#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/KanaText.h"
#include "Core/TypoModel.h"
#include "Core/Japanese/KeyConverter.h"
#include "Core/Japanese/Loanwords.h"
#include "Core/Japanese/RomajiTable.h"

#include <algorithm>
#include <set>
#include <cwctype>
#include <limits>

namespace tekito::japanese {
namespace {

std::wstring ChangeCase(std::wstring text, int letterCase) {
    switch (letterCase) {
    case 1:
        for (auto& ch : text) ch = static_cast<wchar_t>(std::towupper(ch));
        break;
    case 2: {
        bool first = true;
        for (auto& ch : text) {
            ch = static_cast<wchar_t>(first ? std::towupper(ch) : std::towlower(ch));
            first = false;
        }
        break;
    }
    case 3:
        for (auto& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
        break;
    default:
        break;
    }
    return text;
}

// Letters the romaji table left as they were (shown full-width).
std::size_t LeftoverLetters(std::wstring_view reading) {
    return static_cast<std::size_t>(std::count_if(reading.begin(), reading.end(), [](wchar_t c) {
        return (c >= L'\xFF21' && c <= L'\xFF3A') || (c >= L'\xFF41' && c <= L'\xFF5A');
    }));
}

// Slip candidates for the list (AddSlipCandidates). Any letter may have been
// left out here: the first choice only undoes the likelier slips.
constexpr std::wstring_view kSlipDroppedKeys = L"aiueonkgsztdhbpmyrwjfc";
constexpr std::size_t kSlipPhraseKeys = 16;
// A whole input up to this many keys gets this many whole-input readings.
constexpr std::size_t kWholeSlipKeys = 20;
constexpr std::size_t kWholeSlipCandidates = 2;
constexpr std::size_t kWholeSlipExamined = 6;
constexpr std::size_t kSlipCandidates = 4;
// The likeliest readings get this many spellings each; the rest one.
constexpr std::size_t kSlipSpelledReadings = 2;
constexpr std::size_t kSlipSpellings = 2;
constexpr std::size_t kSlipPosition = 5;
// Costs in the connection matrix's units, against the phrase as typed.
constexpr std::int64_t kSlipMargin = 3000;
constexpr std::int64_t kSlipLikelier = 2000;
// Loanwords (AddLoanwords): the first candidates that may start in
// katakana, how long the katakana must be, and the English words offered.
constexpr std::size_t kLoanwordScan = 5;
// Content words of the text committed lately kept for the language model.
constexpr std::size_t kContextWords = 8;
constexpr std::size_t kShortestLoanword = 2;
constexpr std::size_t kLoanwords = 2;

bool IsAlphanumeric(KanaForm form) {
    return form == KanaForm::FullWidthAlphanumeric || form == KanaForm::HalfWidthAlphanumeric;
}

bool IsDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'\xFF10' && c <= L'\xFF19');
}

bool IsDigits(std::wstring_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), IsDigit);
}

// The reading a phrase is learned under. Right after digits it is learned
// apart ("#こ"): 3こ stays 3個 even when こ alone is usually 子. The mark
// keeps it out of predictions, which look up typed kana.
std::wstring LearningReading(std::wstring_view reading, std::size_t begin, std::size_t length) {
    std::wstring key(reading.substr(begin, length));
    if (begin > 0 && IsDigit(reading[begin - 1]) && !key.empty() && !IsDigit(key.front())) key.insert(0, 1, L'#');
    return key;
}

// A number in digits for a word read in kana (いち: 1, the dictionary's
// likeliest) comes after the first two words.
constexpr std::size_t kDigitsPosition = 2;
void PutDigitsAfterWords(std::vector<PhraseCandidate>& candidates) {
    std::vector<PhraseCandidate> digits;
    for (std::size_t i = 0; i < std::min(candidates.size(), kDigitsPosition); ++i) {
        if (candidates[i].kind == PhraseCandidate::Kind::Dictionary && IsDigits(candidates[i].text)) {
            digits.push_back(std::move(candidates[i]));
            candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(i--));
        }
    }
    const auto at = candidates.begin() + static_cast<std::ptrdiff_t>(std::min(candidates.size(), kDigitsPosition));
    candidates.insert(at, std::make_move_iterator(digits.begin()), std::make_move_iterator(digits.end()));
}

}  // namespace

void JapaneseComposer::SetTable(const RomajiTable* table) noexcept {
    table_ = table;
    Clear();
}

void JapaneseComposer::SetInputForm(KanaForm form) noexcept {
    inputForm_ = form == KanaForm::Katakana ? KanaForm::Katakana : KanaForm::Hiragana;
}

void JapaneseComposer::Insert(wchar_t key) {
    // The caller commits a converted text before typing on; if it did not,
    // typing continues from the typed kana.
    phrases_.clear();
    listOpen_ = false;
    Feed(key);
    UpdatePredictions();
}

void JapaneseComposer::UpdatePredictions() {
    predictions_.clear();
    chosenPrediction_.reset();
    if (!predictionEnabled_ || IsConverted()) return;
    // Complete kana only (no key waiting to become one), at least two of
    // them, typed at the end.
    const std::wstring reading = Reading();
    if (!pending_.empty() || reading.size() < 2 || LeftoverLetters(reading) > 0 || caret_ != units_.size()) {
        return;
    }
    constexpr std::size_t kPredictions = 5;
    if (learning_) {
        for (auto& entry : learning_->StartingWith(reading, kPredictions)) {
            predictions_.push_back({std::move(entry.reading), std::move(entry.surface), 0});
        }
    }
    if (converter_) {
        for (auto& prediction : converter_->Predict(reading, kPredictions, userDictionary_)) {
            if (predictions_.size() >= kPredictions) break;
            const bool seen = std::any_of(predictions_.begin(), predictions_.end(),
                                          [&](const Prediction& p) { return p.text == prediction.text; });
            if (!seen) predictions_.push_back(std::move(prediction));
        }
    }
}

void JapaneseComposer::NextPrediction() {
    if (predictions_.empty()) return;
    chosenPrediction_ = chosenPrediction_ ? (*chosenPrediction_ + 1) % predictions_.size() : 0;
}

void JapaneseComposer::PreviousPrediction() {
    if (predictions_.empty()) return;
    const std::size_t count = predictions_.size();
    chosenPrediction_ = chosenPrediction_ ? (*chosenPrediction_ + count - 1) % count : count - 1;
}

void JapaneseComposer::ChoosePrediction(std::size_t index) {
    if (index < predictions_.size()) chosenPrediction_ = index;
}

void JapaneseComposer::Feed(wchar_t key) {
    if (!table_) {
        Emit(std::wstring(1, key), std::wstring(1, key));
        return;
    }
    // Each pass either settles the key or shortens the pending keys, so this
    // ends; the bound only guards against a malformed table.
    for (int guard = 0; guard < 64; ++guard) {
        std::wstring keys = pending_ + key;
        if (table_->HasLongerInput(keys)) {
            pending_ = std::move(keys);
            return;
        }
        if (const RomajiRule* rule = table_->Find(keys)) {
            const std::size_t kept = keys.ends_with(rule->pending) ? rule->pending.size() : 0;
            Emit(keys.substr(0, keys.size() - kept), rule->output);
            pending_ = rule->pending;
            return;
        }
        if (pending_.empty()) {
            // Not in the table at all (a capital letter, say): as typed.
            Emit(std::wstring(1, key), std::wstring(1, key));
            return;
        }
        FlushOnce();
    }
    Emit(std::wstring(1, key), std::wstring(1, key));
}

// Settles the pending keys as far as the table allows: the longest leading
// run that is a row becomes its kana, and the rest is typed again.
void JapaneseComposer::FlushOnce() {
    if (pending_.empty()) return;
    const std::wstring pending = std::move(pending_);
    pending_.clear();
    for (std::size_t length = pending.size(); length > 0; --length) {
        const std::wstring_view head(pending.data(), length);
        const RomajiRule* rule = table_ ? table_->Find(head) : nullptr;
        // A rule that leaves keys pending cannot settle anything here.
        if (!rule || !rule->pending.empty()) continue;
        Emit(std::wstring(head), rule->output);
        for (wchar_t rest : std::wstring_view(pending).substr(length)) Feed(rest);
        return;
    }
    Emit(pending.substr(0, 1), pending.substr(0, 1));
    for (wchar_t rest : std::wstring_view(pending).substr(1)) Feed(rest);
}

void JapaneseComposer::FlushAll() {
    for (int guard = 0; guard < 64 && !pending_.empty(); ++guard) FlushOnce();
    if (!pending_.empty()) {
        Emit(pending_, pending_);
        pending_.clear();
    }
}

void JapaneseComposer::Emit(std::wstring keys, std::wstring kana) {
    units_.insert(units_.begin() + static_cast<std::ptrdiff_t>(caret_), {std::move(keys), std::move(kana)});
    ++caret_;
}

std::size_t JapaneseComposer::ReadingOffset(std::size_t units) const {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < units && i < units_.size(); ++i) offset += units_[i].kana.size();
    return offset;
}

std::size_t JapaneseComposer::SplitAt(std::size_t offset) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < units_.size(); ++i) {
        if (at == offset) return i;
        const std::size_t size = units_[i].kana.size();
        if (offset < at + size) {
            // "きゃ" split into "き" and "ゃ", each with the keys that type it.
            Unit head{{}, units_[i].kana.substr(0, offset - at)};
            Unit tail{{}, units_[i].kana.substr(offset - at)};
            for (Unit* part : {&head, &tail}) {
                auto keys = table_ ? table_->KeysFor(part->kana) : std::wstring{};
                part->keys = keys.empty() ? part->kana : std::move(keys);
            }
            units_[i] = std::move(head);
            units_.insert(units_.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(tail));
            return i + 1;
        }
        at += size;
    }
    return units_.size();
}

void JapaneseComposer::MoveCaret(int delta) {
    if (IsConverted() || !IsComposing()) return;
    FlushAll();
    predictions_.clear();
    chosenPrediction_.reset();
    const auto total = static_cast<long long>(ReadingOffset(units_.size()));
    const auto target = std::clamp(static_cast<long long>(ReadingOffset(caret_)) + delta, 0LL, total);
    caret_ = SplitAt(static_cast<std::size_t>(target));
}

std::size_t JapaneseComposer::CaretOffset() const {
    if (IsConverted()) return Preedit().size();
    return ReadingOffset(caret_) + pending_.size();
}

void JapaneseComposer::Delete() {
    if (IsConverted() || !IsComposing()) return;
    FlushAll();
    if (caret_ >= units_.size()) return;
    auto& next = units_[caret_];
    next.kana.erase(0, 1);
    if (next.kana.empty()) {
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_));
        return;
    }
    auto keys = table_ ? table_->KeysFor(next.kana) : std::wstring{};
    next.keys = keys.empty() ? next.kana : std::move(keys);
}

void JapaneseComposer::Backspace() {
    if (IsConverted()) {
        Cancel();
        return;
    }
    if (!pending_.empty()) {
        pending_.pop_back();
        UpdatePredictions();
        return;
    }
    if (caret_ == 0) return;
    auto& last = units_[caret_ - 1];
    last.kana.pop_back();
    if (last.kana.empty()) {
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_) - 1);
        --caret_;
        UpdatePredictions();
        return;
    }
    // "きゃ" lost its "ゃ": keep what "き" is typed as, for F9 and F10.
    auto keys = table_ ? table_->KeysFor(last.kana) : std::wstring{};
    last.keys = keys.empty() ? last.kana : std::move(keys);
    UpdatePredictions();
}

void JapaneseComposer::Cancel() {
    if (IsConverted()) {
        phrases_.clear();
        listOpen_ = false;
        return;
    }
    Clear();
}

std::wstring JapaneseComposer::Reading() const {
    std::wstring kana;
    for (const auto& unit : units_) kana += unit.kana;
    // Keys the table did not turn into kana are full-width, as in Microsoft
    // IME ("ｋ" while "ka" is being typed).
    return ApplyTypingStyle(ToFullWidthAscii(kana));
}

std::vector<PhraseCandidate> JapaneseComposer::KanaCandidates(std::wstring_view reading) const {
    PhraseCandidate hiragana{std::wstring(reading), 0, PhraseCandidate::Kind::Hiragana, false};
    PhraseCandidate katakana{ToKatakana(reading), 0, PhraseCandidate::Kind::Katakana, false};
    // Space gives the other kana than the one being typed.
    if (inputForm_ == KanaForm::Katakana) return {hiragana, katakana};
    return {katakana, hiragana};
}

void JapaneseComposer::BuildPhrases(bool convert) {
    FlushAll();
    predictions_.clear();
    chosenPrediction_.reset();
    // Back from a conversion, typing goes on at the end.
    caret_ = units_.size();
    const std::wstring reading = Reading();
    conversionReading_ = reading;
    readingKeys_.assign(reading.size() + 1, std::wstring::npos);
    for (std::size_t offset = 0, key = 0, i = 0;; ++i) {
        if (offset < readingKeys_.size()) readingKeys_[offset] = key;
        if (i == units_.size()) break;
        offset += units_[i].kana.size();
        key += units_[i].keys.size();
    }
    phrases_.clear();
    focus_ = 0;
    listOpen_ = false;
    if (reading.empty()) return;
    if (convert) {
        if (auto sum = SumCandidates(reading); !sum.empty()) {
            phrases_.push_back({0, reading.size(), std::move(sum)});
            return;
        }
    }
    if (convert && converter_) {
        // A slip in the keys: converted from the keys as meant. Esc still
        // goes back to what was typed.
        if (auto conversion = keyConverter_ ? keyConverter_->Convert(Keys(false), context_, contextWords_, userDictionary_)
                                            : std::nullopt) {
            conversionReading_ = ApplyTypingStyle(std::move(conversion->reading));
            readingKeys_ = std::move(conversion->keyAt);
            AddPhrases(std::move(conversion->phrases), conversionReading_);
        } else {
            AddPhrases(converter_->Convert(reading, {}, context_, contextWords_, userDictionary_), reading);
        }
    }
    if (phrases_.empty()) {
        phrases_.push_back({0, reading.size(),
                            convert ? KanaCandidates(reading) : std::vector<PhraseCandidate>{}});
        if (convert) AddSpecial(reading, phrases_.back().candidates);
    }
}

void JapaneseComposer::AddPhrases(std::vector<Phrase> phrases, const std::wstring& reading) {
    for (auto& phrase : phrases) {
        if (phrase.candidates.empty()) continue;
        const auto phraseReading = std::wstring_view(reading).substr(phrase.begin, phrase.length);
        if (std::none_of(phraseReading.begin(), phraseReading.end(), IsDigit)) PutDigitsAfterWords(phrase.candidates);
        if (learning_) learning_->Reorder(LearningReading(reading, phrase.begin, phrase.length), phrase.candidates);
        AddLoanwords(phrase.candidates);
        AddSpecial(std::wstring_view(reading).substr(phrase.begin, phrase.length), phrase.candidates);
        phrases_.push_back({phrase.begin, phrase.length, std::move(phrase.candidates)});
    }
}

std::vector<RomajiToken> JapaneseComposer::ParseRomaji(const RomajiTable& table, std::wstring_view keys) {
    JapaneseComposer typing(&table);
    for (const wchar_t key : keys) typing.Feed(key);
    typing.FlushAll();
    std::vector<RomajiToken> tokens;
    tokens.reserve(typing.units_.size());
    for (auto& unit : typing.units_) {
        const bool leftover = LeftoverLetters(ToFullWidthAscii(unit.kana)) > 0;
        tokens.push_back({unit.keys.size(), std::move(unit.kana), leftover});
    }
    return tokens;
}

void JapaneseComposer::AddLoanwords(std::vector<PhraseCandidate>& candidates) const {
    if (!loanwords_) return;
    const auto isKatakana = [](wchar_t c) { return (c >= L'\x30A1' && c <= L'\x30F6') || c == L'\x30FC'; };
    for (std::size_t i = 0; i < std::min(candidates.size(), kLoanwordScan); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.kind != PhraseCandidate::Kind::Dictionary) continue;
        std::size_t length = 0;
        while (length < candidate.text.size() && isKatakana(candidate.text[length])) ++length;
        if (length < kShortestLoanword) continue;
        // The word in English, and what follows it as it was ("ミーティングが"
        // -> "meetingが").
        const std::wstring rest = candidate.text.substr(length);
        std::vector<PhraseCandidate> words;
        for (const auto& word : loanwords_->Words(std::wstring_view(candidate.text).substr(0, length), kLoanwords)) {
            PhraseCandidate english{word + rest, candidate.cost, PhraseCandidate::Kind::English, false};
            const auto same = [&](const PhraseCandidate& c) { return c.text == english.text; };
            if (std::none_of(candidates.begin(), candidates.end(), same)) words.push_back(std::move(english));
        }
        candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(i + 1), words.begin(), words.end());
        return;
    }
}

void JapaneseComposer::AddSpecial(std::wstring_view reading, std::vector<PhraseCandidate>& candidates) const {
    if (!special_ || candidates.empty()) return;
    const auto has = [&](const std::wstring& text) {
        return std::any_of(candidates.begin(), candidates.end(), [&](const PhraseCandidate& c) { return c.text == text; });
    };
    const auto insert = [&](std::size_t at, const std::vector<std::wstring>& texts, std::wstring_view suffix) {
        at = std::min(at, candidates.size());
        for (const auto& text : texts) {
            PhraseCandidate candidate{text + std::wstring(suffix), candidates.front().cost,
                                      PhraseCandidate::Kind::Special, false};
            if (has(candidate.text)) continue;
            candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(at++), std::move(candidate));
        }
    };
    // Other forms of a number come right after it, and after a number with
    // a counter as the first candidate writes it (３個: 3個, 三個).
    if (specialOptions_.numbers) {
        std::size_t digits = 0;
        while (digits < reading.size() && IsDigit(reading[digits])) ++digits;
        const auto& first = candidates.front().text;
        if (digits == reading.size()) {
            insert(1, special_->Numbers(reading), {});
        } else if (digits > 0 && first.size() > digits && !IsDigit(first[digits]) &&
                   ToHalfWidthAscii(first.substr(0, digits)) == ToHalfWidthAscii(reading.substr(0, digits))) {
            const std::wstring rest = first.substr(digits);
            if (rest.front() != L'.' && rest.front() != L',' && rest.front() != L'\xFF0E' && rest.front() != L'\xFF0C') {
                insert(1, special_->Numbers(reading.substr(0, digits)), rest);
            }
        }
    }
    // A date or time word, and what follows it in the phrase as long as the
    // first candidate ends with it too ("きょうは": 今日は, 2026/09/29は).
    if (specialOptions_.dates) {
        const auto now = LocalTime::Now();
        for (std::size_t length = reading.size(); length >= 2; --length) {
            const auto suffix = reading.substr(length);
            if (!candidates.front().text.ends_with(suffix)) continue;
            auto dates = special_->Dates(SpecialConversions::Language::Japanese, reading.substr(0, length), now);
            if (dates.empty()) continue;
            insert(1, dates, suffix);
            break;
        }
    }
    // Symbols after the first few words ("やじるし": 矢印, ..., →).
    constexpr std::size_t kSymbolPosition = 3;
    if (specialOptions_.symbols) {
        insert(kSymbolPosition, special_->Symbols(SpecialConversions::Language::Japanese, reading), {});
    }
}

std::vector<PhraseCandidate> JapaneseComposer::SumCandidates(std::wstring_view reading) const {
    if (!special_ || !specialOptions_.calculator) return {};
    const auto sum = SpecialConversions::Calculate(reading);
    if (!sum) return {};
    // The arithmetic as ASCII, the way it was meant (ー is minus, ・ divide).
    std::wstring expression = ToHalfWidthAscii(reading);
    for (auto& c : expression) {
        if (c == L'\x30FC') c = L'-';
        if (c == L'\x30FB') c = L'/';
    }
    std::vector<PhraseCandidate> candidates{{*sum, 0, PhraseCandidate::Kind::Special, false},
                                            {expression + *sum, 0, PhraseCandidate::Kind::Special, false}};
    for (auto& kana : KanaCandidates(reading)) {
        const bool seen = std::any_of(candidates.begin(), candidates.end(),
                                      [&](const PhraseCandidate& c) { return c.text == kana.text; });
        if (!seen) candidates.push_back(std::move(kana));
    }
    return candidates;
}

void JapaneseComposer::Convert() {
    if (!IsComposing()) return;
    if (!IsConverted()) {
        BuildPhrases(true);
        return;
    }
    NextCandidate();
}

std::vector<std::pair<PhraseCandidate, std::wstring>> JapaneseComposer::SlipReadings(
    const std::wstring& keys, std::set<std::wstring> skip, std::int64_t typedCost) const {
    // Every slip: a neighboring key, a key dropped (any letter), an extra
    // key, two keys swapped.
    std::vector<std::wstring> variants;
    for (std::size_t at = 0; at <= keys.size(); ++at) {
        for (const wchar_t missing : kSlipDroppedKeys) {
            std::wstring variant = keys;
            variant.insert(at, 1, missing);
            variants.push_back(std::move(variant));
        }
        if (at == keys.size()) break;
        for (wchar_t other = L'a'; other <= L'z'; ++other) {
            if (!AreQwertyNeighbors(keys[at], other)) continue;
            std::wstring variant = keys;
            variant[at] = other;
            variants.push_back(std::move(variant));
        }
        std::wstring removed = keys;
        removed.erase(at, 1);
        variants.push_back(std::move(removed));
        if (at + 1 < keys.size() && keys[at] != keys[at + 1]) {
            std::wstring swapped = keys;
            std::swap(swapped[at], swapped[at + 1]);
            variants.push_back(std::move(swapped));
        }
    }
    std::vector<std::pair<PhraseCandidate, std::wstring>> found;  // with its kana
    for (const auto& variant : variants) {
        std::wstring kana;
        bool readable = !variant.empty();
        for (const auto& token : ParseRomaji(*table_, variant)) {
            readable = readable && !token.leftover;
            kana += token.kana;
        }
        if (!readable) continue;
        kana = ApplyTypingStyle(std::move(kana));
        if (!skip.insert(kana).second) continue;
        auto candidate = converter_->Best(kana, 0, userDictionary_);
        if (candidate && !candidate->spellingCorrection && candidate->cost <= typedCost + kSlipMargin) {
            found.emplace_back(std::move(*candidate), std::move(kana));
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first.cost < b.first.cost; });
    return found;
}

void JapaneseComposer::AddSlipCandidates(PhraseState& phrase) const {
    if (phrase.slipsAdded) return;
    phrase.slipsAdded = true;
    if (!converter_ || !table_) return;
    const auto lettersOnly = [](const std::wstring& keys) {
        return std::all_of(keys.begin(), keys.end(), [](wchar_t c) { return c >= L'a' && c <= L'z'; });
    };
    const auto typedKanaOf = [&](const std::wstring& keys) {
        std::wstring kana;
        for (const auto& token : ParseRomaji(*table_, keys)) kana += token.kana;
        return ApplyTypingStyle(ToFullWidthAscii(kana));
    };
    const auto costOf = [&](const std::optional<PhraseCandidate>& candidate) {
        return candidate ? candidate->cost : std::numeric_limits<std::int64_t>::max() / 4;
    };

    std::vector<PhraseCandidate> added;
    bool likelier = false;
    const auto offer = [&](PhraseCandidate candidate) {
        const auto same = [&](const PhraseCandidate& c) { return c.text == candidate.text; };
        if (std::any_of(phrase.candidates.begin(), phrase.candidates.end(), same) ||
            std::any_of(added.begin(), added.end(), same)) {
            return false;
        }
        candidate.slip = true;
        added.push_back(std::move(candidate));
        return true;
    };

    // A short input split into phrases: the whole input with a slip undone,
    // when the slip is what made the phrases (はじめまして typed hahimemashite
    // is は|秘めまして). A slip inside one phrase is left to that phrase's
    // own list below. These go after the phrase's own candidates.
    const std::wstring allKeys = Keys(false);
    std::vector<PhraseCandidate> wholeInput;
    if (phrases_.size() > 1 && allKeys.size() <= kWholeSlipKeys && lettersOnly(allKeys)) {
        const std::wstring typedAll = typedKanaOf(allKeys);
        const std::int64_t typedCost = costOf(converter_->Best(typedAll, 0, userDictionary_));
        std::vector<std::size_t> starts;
        for (const auto& p : phrases_) starts.push_back(p.begin);
        std::size_t examined = 0;
        for (auto& [candidate, kana] : SlipReadings(allKeys, {typedAll, conversionReading_}, typedCost)) {
            if (wholeInput.size() >= kWholeSlipCandidates || examined++ >= kWholeSlipExamined) break;
            if (SamePhrasing(conversionReading_, starts, kana)) continue;
            candidate.reading = kana;
            candidate.slip = true;
            wholeInput.push_back(std::move(candidate));
        }
    }

    // The keys the phrase was read from (when it starts and ends between
    // units or words).
    if (phrase.begin + phrase.length >= readingKeys_.size()) return;
    const std::size_t firstKey = readingKeys_[phrase.begin];
    const std::size_t endKey = readingKeys_[phrase.begin + phrase.length];
    std::optional<PhraseCandidate> asTyped;
    const std::wstring keys = firstKey == std::wstring::npos || endKey == std::wstring::npos || endKey <= firstKey
                                  ? std::wstring{}
                                  : allKeys.substr(firstKey, endKey - firstKey);
    if (keys.size() >= 2 && keys.size() <= kSlipPhraseKeys && lettersOnly(keys)) {
        // The kana as typed; when a slip was corrected for the first choice,
        // they are not the phrase's reading, and what they convert to is
        // offered right after it.
        const std::wstring shown = conversionReading_.substr(phrase.begin, phrase.length);
        const std::wstring typedKana = typedKanaOf(keys);
        auto typed = converter_->Best(typedKana, 0, userDictionary_);
        if (typed && typed->spellingCorrection) typed.reset();
        const std::int64_t typedCost = costOf(typed);
        if (typedKana != shown) {
            asTyped = typed ? std::move(typed) : std::optional<PhraseCandidate>{KanaCandidates(typedKana).back()};
        }
        // The likeliest readings with a slip undone, each with its likeliest
        // spellings (きかいが: 機会が, 機械が), then the next readings.
        const auto found = SlipReadings(keys, {typedKana, shown}, typedCost);
        std::size_t offered = 0;
        for (std::size_t i = 0; i < found.size() && offered < kSlipCandidates; ++i) {
            likelier = likelier || (i == 0 && found[i].first.cost + kSlipLikelier < typedCost);
            if (i >= kSlipSpelledReadings) {
                if (offer(found[i].first)) ++offered;
                continue;
            }
            const std::size_t whole[] = {found[i].second.size()};
            const auto phrases = converter_->Convert(found[i].second, whole, 0, {}, userDictionary_);
            std::size_t spellings = 0;
            for (std::size_t c = 0; phrases.size() == 1 && c < phrases.front().candidates.size() &&
                                    spellings < kSlipSpellings && offered < kSlipCandidates;
                 ++c) {
                const auto& candidate = phrases.front().candidates[c];
                if (candidate.kind != PhraseCandidate::Kind::Dictionary || candidate.spellingCorrection) continue;
                if (offer(candidate)) ++offered;
                ++spellings;
            }
        }
    }
    // Right after the first choice when a slip reads much more likely (or
    // the first choice is already a correction), otherwise after the first
    // few candidates.
    if (asTyped && std::none_of(phrase.candidates.begin(), phrase.candidates.end(),
                                [&](const PhraseCandidate& c) { return c.text == asTyped->text; })) {
        added.insert(added.begin(), std::move(*asTyped));
        likelier = true;
    }
    // Whole-input readings never come right after the first choice (the
    // next Space would show them): after the phrase's own when those do,
    // otherwise first at kSlipPosition, as the likelier sign of a slip.
    std::erase_if(wholeInput, [&](const PhraseCandidate& candidate) {
        const auto same = [&](const PhraseCandidate& c) { return c.text == candidate.text; };
        return std::any_of(phrase.candidates.begin(), phrase.candidates.end(), same) ||
               std::any_of(added.begin(), added.end(), same);
    });
    if (likelier) {
        added.insert(added.end(), std::make_move_iterator(wholeInput.begin()), std::make_move_iterator(wholeInput.end()));
    } else {
        added.insert(added.begin(), std::make_move_iterator(wholeInput.begin()), std::make_move_iterator(wholeInput.end()));
    }
    if (added.empty()) return;
    const std::size_t at = std::min(phrase.candidates.size(), likelier ? std::size_t{1} : kSlipPosition);
    phrase.candidates.insert(phrase.candidates.begin() + static_cast<std::ptrdiff_t>(at),
                             std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    if (phrase.selected >= at) phrase.selected += added.size();
}

bool JapaneseComposer::SamePhrasing(const std::wstring& reading, const std::vector<std::size_t>& starts,
                                    const std::wstring& corrected) const {
    // Where the two readings differ, and the phrases the corrected one makes.
    std::size_t prefix = 0;
    while (prefix < reading.size() && prefix < corrected.size() && reading[prefix] == corrected[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (suffix < reading.size() - prefix && suffix < corrected.size() - prefix &&
           reading[reading.size() - 1 - suffix] == corrected[corrected.size() - 1 - suffix]) {
        ++suffix;
    }
    std::vector<std::size_t> correctedStarts;
    for (const auto& phrase : converter_->Convert(corrected, {}, 0, {}, userDictionary_)) correctedStarts.push_back(phrase.begin);
    // Phrase starts outside the difference, in the reading's positions; one
    // inside it is phrased differently.
    const auto mapped = [&](const std::vector<std::size_t>& list, std::size_t size) {
        std::vector<std::size_t> out;
        for (const std::size_t start : list) {
            if (start > prefix && start < size - suffix) return std::optional<std::vector<std::size_t>>{};
            out.push_back(start <= prefix ? start : start - size + reading.size());
        }
        return std::optional<std::vector<std::size_t>>{out};
    };
    const auto before = mapped(starts, reading.size());
    const auto after = mapped(correctedStarts, corrected.size());
    return before && after && *before == *after;
}

void JapaneseComposer::NextCandidate() {
    if (!IsConverted()) {
        Convert();
        return;
    }
    auto& phrase = phrases_[focus_];
    AddSlipCandidates(phrase);
    if (phrase.candidates.empty()) {
        phrase.candidates = KanaCandidates(conversionReading_.substr(phrase.begin, phrase.length));
    }
    if (!phrase.form) phrase.selected = (phrase.selected + 1) % phrase.candidates.size();
    phrase.form.reset();
    listOpen_ = true;
}

void JapaneseComposer::PreviousCandidate() {
    if (!IsConverted()) {
        Convert();
        return;
    }
    auto& phrase = phrases_[focus_];
    AddSlipCandidates(phrase);
    if (phrase.candidates.empty()) {
        phrase.candidates = KanaCandidates(conversionReading_.substr(phrase.begin, phrase.length));
    }
    const std::size_t count = phrase.candidates.size();
    if (!phrase.form) phrase.selected = (phrase.selected + count - 1) % count;
    phrase.form.reset();
    listOpen_ = true;
}

void JapaneseComposer::SelectCandidate(std::size_t index) {
    if (!IsConverted()) return;
    auto& phrase = phrases_[focus_];
    if (index >= phrase.candidates.size()) return;
    phrase.selected = index;
    phrase.form.reset();
    listOpen_ = false;
}

void JapaneseComposer::MoveFocus(int delta) {
    if (!IsConverted()) return;
    listOpen_ = false;
    const auto count = static_cast<int>(phrases_.size());
    focus_ = static_cast<std::size_t>(std::clamp(static_cast<int>(focus_) + delta, 0, count - 1));
}

void JapaneseComposer::ResizeFocus(int delta) {
    if (!IsConverted() || !converter_) return;
    const std::wstring reading = conversionReading_;
    const auto& focused = phrases_[focus_];
    const auto length = static_cast<long long>(focused.length) + delta;
    if (length < 1 || focused.begin + static_cast<std::size_t>(length) > reading.size()) return;

    // Phrases before the focus keep their length and choice; the focused one
    // takes its new length; the rest is split and converted again.
    std::vector<std::size_t> fixed;
    for (std::size_t i = 0; i < focus_; ++i) fixed.push_back(phrases_[i].length);
    fixed.push_back(static_cast<std::size_t>(length));
    std::vector<PhraseState> kept(phrases_.begin(), phrases_.begin() + static_cast<long long>(focus_));

    phrases_.clear();
    AddPhrases(converter_->Convert(reading, fixed, context_, contextWords_, userDictionary_), reading);
    if (phrases_.size() <= focus_) {
        BuildPhrases(true);
        return;
    }
    for (std::size_t i = 0; i < kept.size(); ++i) phrases_[i] = std::move(kept[i]);
    listOpen_ = false;
}

void JapaneseComposer::CycleKana() {
    if (!IsComposing()) return;
    if (!IsConverted()) BuildPhrases(false);
    auto& phrase = phrases_[focus_];
    if (!phrase.form || IsAlphanumeric(*phrase.form)) {
        // First press: the other kana than the one being typed.
        phrase.form = inputForm_ == KanaForm::Katakana ? KanaForm::Hiragana : KanaForm::Katakana;
    } else if (*phrase.form == KanaForm::Hiragana) {
        phrase.form = KanaForm::Katakana;
    } else if (*phrase.form == KanaForm::Katakana) {
        phrase.form = KanaForm::HalfWidthKatakana;
    } else {
        phrase.form = KanaForm::Hiragana;
    }
    listOpen_ = false;
}

void JapaneseComposer::Transliterate(KanaForm form) {
    if (!IsComposing()) return;
    if (!IsConverted()) BuildPhrases(false);
    auto& phrase = phrases_[focus_];
    phrase.letterCase = IsAlphanumeric(form) && phrase.form == form ? (phrase.letterCase + 1) % 4 : 0;
    phrase.form = form;
    listOpen_ = false;
}

std::wstring JapaneseComposer::Keys(bool includePending) const {
    std::wstring keys;
    for (const auto& unit : units_) keys += unit.keys;
    if (includePending) keys += pending_;
    return keys;
}

std::wstring JapaneseComposer::KeysFor(std::size_t begin, std::size_t length) const {
    std::wstring keys;
    std::size_t at = 0;
    for (const auto& unit : units_) {
        const std::size_t end = at + unit.kana.size();
        if (end > begin && at < begin + length) keys += unit.keys;
        at = end;
    }
    return keys;
}

std::wstring JapaneseComposer::ApplyTypingStyle(std::wstring text) const {
    const bool comma = punctuation_ == PunctuationStyle::CommaPeriod ||
                       punctuation_ == PunctuationStyle::CommaKuten;
    const bool period = punctuation_ == PunctuationStyle::CommaPeriod ||
                        punctuation_ == PunctuationStyle::ToutenPeriod;
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto& ch = text[i];
        if (halfWidthDigits_ && ch >= L'\xFF10' && ch <= L'\xFF19') ch = static_cast<wchar_t>(ch - 0xFF10 + L'0');
        // Between digits: a decimal point or a thousands comma.
        const bool betweenDigits = i > 0 && i + 1 < text.size() && IsDigit(text[i - 1]) && IsDigit(text[i + 1]);
        if (betweenDigits && (ch == L'。' || ch == L'．')) {
            ch = halfWidthDigits_ ? L'.' : L'．';
        } else if (betweenDigits && (ch == L'、' || ch == L'，')) {
            ch = halfWidthDigits_ ? L',' : L'，';
        } else if (comma && ch == L'、') {
            ch = L'，';
        } else if (period && ch == L'。') {
            ch = L'．';
        }
    }
    return text;
}

std::wstring JapaneseComposer::RenderTyping(bool includePending) const {
    std::wstring kana = Reading();
    // Pending keys show at the caret, where the kana they make will go.
    if (includePending) kana.insert(ReadingOffset(caret_), ToFullWidthAscii(pending_));
    return inputForm_ == KanaForm::Katakana ? ToKatakana(kana) : kana;
}

std::wstring JapaneseComposer::FormText(KanaForm form, std::size_t begin, std::size_t length,
                                        int letterCase) const {
    const std::wstring reading = conversionReading_.substr(begin, length);
    switch (form) {
    case KanaForm::Hiragana: return ToHiragana(reading);
    case KanaForm::Katakana: return ToKatakana(reading);
    case KanaForm::HalfWidthKatakana: return ToHalfWidthKatakana(reading);
    case KanaForm::FullWidthAlphanumeric:
        return ToFullWidthAscii(ChangeCase(KeysFor(begin, length), letterCase));
    case KanaForm::HalfWidthAlphanumeric:
        return ChangeCase(KeysFor(begin, length), letterCase);
    }
    return reading;
}

std::wstring JapaneseComposer::PhraseText(const PhraseState& phrase) const {
    if (phrase.form) return FormText(*phrase.form, phrase.begin, phrase.length, phrase.letterCase);
    if (phrase.selected < phrase.candidates.size()) return phrase.candidates[phrase.selected].text;
    return conversionReading_.substr(phrase.begin, phrase.length);
}

const PhraseCandidate* JapaneseComposer::WholeChoice() const noexcept {
    if (!IsConverted()) return nullptr;
    const auto& phrase = phrases_[focus_];
    if (phrase.form || phrase.selected >= phrase.candidates.size()) return nullptr;
    const auto& candidate = phrase.candidates[phrase.selected];
    return candidate.reading.empty() ? nullptr : &candidate;
}

std::wstring JapaneseComposer::Preedit() const {
    if (!IsConverted()) return RenderTyping(true);
    if (const auto* whole = WholeChoice()) return whole->text;
    std::wstring text;
    for (const auto& phrase : phrases_) text += PhraseText(phrase);
    return text;
}

std::vector<PreeditSegment> JapaneseComposer::Segments() const {
    if (!IsConverted()) {
        if (!IsComposing()) return {};
        return {{RenderTyping(true), false, false}};
    }
    if (const auto* whole = WholeChoice()) return {{whole->text, true, true}};
    std::vector<PreeditSegment> segments;
    for (std::size_t i = 0; i < phrases_.size(); ++i) {
        segments.push_back({PhraseText(phrases_[i]), true, i == focus_});
    }
    return segments;
}

const std::vector<PhraseCandidate>* JapaneseComposer::FocusedCandidates() const noexcept {
    if (!IsConverted()) return nullptr;
    return &phrases_[focus_].candidates;
}

std::size_t JapaneseComposer::FocusedSelection() const noexcept {
    return IsConverted() ? phrases_[focus_].selected : 0;
}

std::wstring JapaneseComposer::FocusedReading() const {
    if (!IsConverted()) return {};
    const auto& phrase = phrases_[focus_];
    if (phrase.begin >= conversionReading_.size()) return {};
    return conversionReading_.substr(phrase.begin, phrase.length);
}

std::vector<std::wstring> JapaneseComposer::PhraseKeys() const {
    std::vector<std::wstring> keys;
    const std::wstring typed = Keys(false);
    for (const auto& phrase : phrases_) {
        const std::size_t end = phrase.begin + phrase.length;
        if (end >= readingKeys_.size() || readingKeys_[phrase.begin] == std::wstring::npos ||
            readingKeys_[end] == std::wstring::npos || readingKeys_[end] < readingKeys_[phrase.begin]) {
            keys.emplace_back();
            continue;
        }
        keys.push_back(typed.substr(readingKeys_[phrase.begin], readingKeys_[end] - readingKeys_[phrase.begin]));
    }
    return keys;
}

std::wstring JapaneseComposer::Commit() {
    FlushAll();
    if (chosenPrediction_ && *chosenPrediction_ < predictions_.size() && !IsConverted()) {
        const auto prediction = predictions_[*chosenPrediction_];
        if (learning_) learning_->RecordChoice(prediction.reading, prediction.text, prediction.text);
        // A prediction's part of speech is not known here.
        context_ = 0;
        RememberWords(prediction.text);
        Clear();
        return prediction.text;
    }
    if (const auto* whole = WholeChoice()) {
        if (learning_) learning_->RecordChoice(whole->reading, whole->text, whole->text);
        auto text = whole->text;
        context_ = ContextAfter(text, whole->rightId);
        RememberWords(text);
        Clear();
        return text;
    }
    if (learning_ && IsConverted()) {
        const std::wstring& reading = conversionReading_;
        for (const auto& phrase : phrases_) {
            // Dates and sums change; they are not learned.
            if (!phrase.form && phrase.selected < phrase.candidates.size() &&
                phrase.candidates[phrase.selected].kind == PhraseCandidate::Kind::Special) {
                continue;
            }
            const std::wstring chosen = PhraseText(phrase);
            const std::wstring first = phrase.candidates.empty() ? chosen : phrase.candidates.front().text;
            learning_->RecordChoice(LearningReading(reading, phrase.begin, phrase.length), chosen, first);
        }
    }
    std::uint16_t last = 0;
    if (IsConverted()) {
        const auto& phrase = phrases_.back();
        if (!phrase.form && phrase.selected < phrase.candidates.size()) last = phrase.candidates[phrase.selected].rightId;
    }
    auto text = IsConverted() ? Preedit() : RenderTyping(false);
    context_ = ContextAfter(text, last);
    RememberWords(text);
    Clear();
    return text;
}

void JapaneseComposer::RememberWords(std::wstring_view text) {
    const auto content = [](wchar_t c) {
        return (c >= 0x4E00 && c <= 0x9FFF) || c == 0x3005 || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC;
    };
    for (std::size_t i = 0; i < text.size();) {
        if (!content(text[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && content(text[end])) ++end;
        if (end - i >= 2) contextWords_.emplace_back(text.substr(i, end - i));
        i = end;
    }
    if (contextWords_.size() > kContextWords) {
        contextWords_.erase(contextWords_.begin(),
                            contextWords_.end() - static_cast<std::ptrdiff_t>(kContextWords));
    }
}

std::uint16_t JapaneseComposer::ContextAfter(std::wstring_view text, std::uint16_t rightId) noexcept {
    // A sentence that ended starts the next one afresh.
    if (text.empty() || std::wstring_view(L"。．.！!？?\n").find(text.back()) != std::wstring_view::npos) return 0;
    return rightId;
}

void JapaneseComposer::Clear() noexcept {
    predictions_.clear();
    chosenPrediction_.reset();
    units_.clear();
    conversionReading_.clear();
    readingKeys_.clear();
    caret_ = 0;
    pending_.clear();
    phrases_.clear();
    focus_ = 0;
    listOpen_ = false;
}

}  // namespace tekito::japanese
