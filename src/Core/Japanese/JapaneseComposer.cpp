#include "Core/Japanese/JapaneseComposer.h"

#include "Core/Japanese/KanaText.h"
#include "Core/Japanese/RomajiTable.h"

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

}  // namespace

void JapaneseComposer::SetTable(const RomajiTable* table) noexcept {
    table_ = table;
    Clear();
}

void JapaneseComposer::SetInputForm(KanaForm form) noexcept {
    inputForm_ = form == KanaForm::Katakana ? KanaForm::Katakana : KanaForm::Hiragana;
    if (!converted_) form_ = inputForm_;
}

void JapaneseComposer::Insert(wchar_t key) {
    if (converted_) {
        // The caller commits a converted text before typing on; if it did
        // not, typing continues from the typed kana.
        converted_ = false;
        form_ = inputForm_;
    }
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
        if (!rule) continue;
        // A rule that leaves keys pending cannot settle anything here.
        if (!rule->pending.empty()) continue;
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
    units_.push_back({std::move(keys), std::move(kana)});
}

void JapaneseComposer::Backspace() {
    if (converted_) {
        converted_ = false;
        form_ = inputForm_;
        return;
    }
    if (!pending_.empty()) {
        pending_.pop_back();
        return;
    }
    if (units_.empty()) return;
    auto& last = units_.back();
    last.kana.pop_back();
    if (last.kana.empty()) {
        units_.pop_back();
        return;
    }
    // "きゃ" lost its "ゃ": keep what "き" is typed as, for F9 and F10.
    auto keys = table_ ? table_->KeysFor(last.kana) : std::wstring{};
    last.keys = keys.empty() ? last.kana : std::move(keys);
}

void JapaneseComposer::Cancel() {
    if (converted_) {
        converted_ = false;
        form_ = inputForm_;
        return;
    }
    Clear();
}

void JapaneseComposer::Convert() {
    if (!IsComposing()) return;
    FlushAll();
    form_ = converted_ && form_ == KanaForm::Katakana ? KanaForm::Hiragana
            : converted_ && form_ == KanaForm::Hiragana ? KanaForm::Katakana
            : inputForm_ == KanaForm::Katakana         ? KanaForm::Hiragana
                                                        : KanaForm::Katakana;
    converted_ = true;
}

void JapaneseComposer::CycleKana() {
    if (!IsComposing()) return;
    FlushAll();
    const bool cycling = converted_ && (form_ == KanaForm::Hiragana || form_ == KanaForm::Katakana ||
                                        form_ == KanaForm::HalfWidthKatakana);
    if (!cycling) {
        // First press: the other kana than the one being typed.
        form_ = inputForm_ == KanaForm::Katakana ? KanaForm::Hiragana : KanaForm::Katakana;
    } else if (form_ == KanaForm::Hiragana) {
        form_ = KanaForm::Katakana;
    } else if (form_ == KanaForm::Katakana) {
        form_ = KanaForm::HalfWidthKatakana;
    } else {
        form_ = KanaForm::Hiragana;
    }
    converted_ = true;
}

void JapaneseComposer::Transliterate(KanaForm form) {
    if (!IsComposing()) return;
    FlushAll();
    const bool alphanumeric = form == KanaForm::FullWidthAlphanumeric ||
                              form == KanaForm::HalfWidthAlphanumeric;
    letterCase_ = alphanumeric && converted_ && form_ == form ? (letterCase_ + 1) % 4 : 0;
    form_ = form;
    converted_ = true;
}

std::wstring JapaneseComposer::Keys(bool includePending) const {
    std::wstring keys;
    for (const auto& unit : units_) keys += unit.keys;
    if (includePending) keys += pending_;
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

std::wstring JapaneseComposer::Render(bool includePending) const {
    switch (form_) {
    case KanaForm::FullWidthAlphanumeric:
        return ToFullWidthAscii(ChangeCase(Keys(includePending), letterCase_));
    case KanaForm::HalfWidthAlphanumeric:
        return ChangeCase(Keys(includePending), letterCase_);
    default:
        break;
    }
    std::wstring kana;
    for (const auto& unit : units_) kana += unit.kana;
    // Keys the table did not turn into kana are shown full-width, as in
    // Microsoft IME ("ｋ" while "ka" is being typed).
    kana = ApplyPunctuation(ToFullWidthAscii(kana));
    if (includePending) kana += ToFullWidthAscii(pending_);
    switch (form_) {
    case KanaForm::Katakana: return ToKatakana(kana);
    case KanaForm::HalfWidthKatakana: return ToHalfWidthKatakana(kana);
    default: return kana;
    }
}

std::wstring JapaneseComposer::Preedit() const {
    return Render(true);
}

std::wstring JapaneseComposer::Commit() {
    FlushAll();
    auto text = Render(false);
    Clear();
    return text;
}

void JapaneseComposer::Clear() noexcept {
    units_.clear();
    pending_.clear();
    converted_ = false;
    form_ = inputForm_;
    letterCase_ = 0;
}

}  // namespace tekito::japanese
