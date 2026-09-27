#include "Core/Japanese/JapaneseComposer.h"

#include "Core/Japanese/JapaneseLearning.h"
#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/RomajiTable.h"

#include <algorithm>
#include <cwctype>

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

bool IsAlphanumeric(KanaForm form) {
    return form == KanaForm::FullWidthAlphanumeric || form == KanaForm::HalfWidthAlphanumeric;
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
        return;
    }
    if (caret_ == 0) return;
    auto& last = units_[caret_ - 1];
    last.kana.pop_back();
    if (last.kana.empty()) {
        units_.erase(units_.begin() + static_cast<std::ptrdiff_t>(caret_) - 1);
        --caret_;
        return;
    }
    // "きゃ" lost its "ゃ": keep what "き" is typed as, for F9 and F10.
    auto keys = table_ ? table_->KeysFor(last.kana) : std::wstring{};
    last.keys = keys.empty() ? last.kana : std::move(keys);
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
    return ApplyPunctuation(ToFullWidthAscii(kana));
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
    // Back from a conversion, typing goes on at the end.
    caret_ = units_.size();
    const std::wstring reading = Reading();
    phrases_.clear();
    focus_ = 0;
    listOpen_ = false;
    if (reading.empty()) return;
    if (convert && converter_) AddPhrases(converter_->Convert(reading), reading);
    if (phrases_.empty()) {
        phrases_.push_back({0, reading.size(),
                            convert ? KanaCandidates(reading) : std::vector<PhraseCandidate>{}});
    }
}

void JapaneseComposer::AddPhrases(std::vector<Phrase> phrases, const std::wstring& reading) {
    for (auto& phrase : phrases) {
        if (phrase.candidates.empty()) continue;
        if (learning_) learning_->Reorder(reading.substr(phrase.begin, phrase.length), phrase.candidates);
        phrases_.push_back({phrase.begin, phrase.length, std::move(phrase.candidates)});
    }
}

void JapaneseComposer::Convert() {
    if (!IsComposing()) return;
    if (!IsConverted()) {
        BuildPhrases(true);
        return;
    }
    NextCandidate();
}

void JapaneseComposer::NextCandidate() {
    if (!IsConverted()) {
        Convert();
        return;
    }
    auto& phrase = phrases_[focus_];
    if (phrase.candidates.empty()) phrase.candidates = KanaCandidates(Reading().substr(phrase.begin, phrase.length));
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
    if (phrase.candidates.empty()) phrase.candidates = KanaCandidates(Reading().substr(phrase.begin, phrase.length));
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
    const std::wstring reading = Reading();
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
    AddPhrases(converter_->Convert(reading, fixed), reading);
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

std::wstring JapaneseComposer::ApplyPunctuation(std::wstring text) const {
    const bool comma = punctuation_ == PunctuationStyle::CommaPeriod ||
                       punctuation_ == PunctuationStyle::CommaKuten;
    const bool period = punctuation_ == PunctuationStyle::CommaPeriod ||
                        punctuation_ == PunctuationStyle::ToutenPeriod;
    for (auto& ch : text) {
        if (comma && ch == L'、') ch = L'，';
        if (period && ch == L'。') ch = L'．';
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
    const std::wstring reading = Reading().substr(begin, length);
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
    return Reading().substr(phrase.begin, phrase.length);
}

std::wstring JapaneseComposer::Preedit() const {
    if (!IsConverted()) return RenderTyping(true);
    std::wstring text;
    for (const auto& phrase : phrases_) text += PhraseText(phrase);
    return text;
}

std::vector<PreeditSegment> JapaneseComposer::Segments() const {
    if (!IsConverted()) {
        if (!IsComposing()) return {};
        return {{RenderTyping(true), false, false}};
    }
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

std::wstring JapaneseComposer::Commit() {
    FlushAll();
    if (learning_ && IsConverted()) {
        const std::wstring reading = Reading();
        for (const auto& phrase : phrases_) {
            const std::wstring chosen = PhraseText(phrase);
            const std::wstring first = phrase.candidates.empty() ? chosen : phrase.candidates.front().text;
            learning_->RecordChoice(reading.substr(phrase.begin, phrase.length), chosen, first);
        }
    }
    auto text = IsConverted() ? Preedit() : RenderTyping(false);
    Clear();
    return text;
}

void JapaneseComposer::Clear() noexcept {
    units_.clear();
    caret_ = 0;
    pending_.clear();
    phrases_.clear();
    focus_ = 0;
    listOpen_ = false;
}

}  // namespace tekito::japanese
