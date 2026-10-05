#include "Core/Japanese/JapaneseKeys.h"

#include <algorithm>

namespace tekito::japanese {
namespace {

using Key = KeyPress::Key;
using Action = KeyCommand::Action;

KeyCommand Make(Action action) {
    KeyCommand command;
    command.action = action;
    return command;
}

KeyCommand WithDelta(Action action, int delta) {
    auto command = Make(action);
    command.delta = delta;
    return command;
}

// Keys that move the caret or focus: they settle the text and go on to the
// application.
bool IsCaretKey(Key key) {
    switch (key) {
    case Key::Left: case Key::Right: case Key::Up: case Key::Down:
    case Key::Home: case Key::End: case Key::PageUp: case Key::PageDown:
    case Key::Delete: case Key::Insert: case Key::Tab:
        return true;
    default:
        return false;
    }
}

}  // namespace

std::optional<KeyCommand> TranslateKey(const JapaneseComposer& composer, const KeyPress& key,
                                       const KeyOptions& options) {
    const bool composing = composer.IsComposing();
    if (!composing && key.key == Key::Backspace && key.control && !key.shift && composer.LastCommit()) {
        return Make(Action::UndoCommit);
    }
    if (composing && key.key == Key::Delete && key.control && !key.shift && composer.CanForgetChosen()) {
        return Make(Action::ForgetChosen);
    }
    if (key.command) {
        // Shortcuts act on the committed text.
        if (!composing) return std::nullopt;
        auto command = Make(Action::Commit);
        command.letKeyThrough = true;
        return command;
    }
    // Predictions while typing (also while the live conversion is shown).
    if (composer.IsTyping() && !composer.Predictions().empty()) {
        const bool chosen = composer.ChosenPrediction().has_value();
        switch (key.key) {
        case Key::Tab:
            return Make(key.shift ? Action::PreviousPrediction : Action::NextPrediction);
        case Key::Down:
            return Make(Action::NextPrediction);
        case Key::Up:
            if (chosen) return Make(Action::PreviousPrediction);
            break;
        case Key::Escape:
            if (chosen) return Make(Action::ClearPrediction);
            break;
        default:
            break;
        }
    }
    if (composer.IsConverted()) {
        switch (key.key) {
        case Key::Space:
            return Make(key.shift ? Action::PreviousCandidate : Action::Convert);
        case Key::Down:
            return Make(Action::NextCandidate);
        case Key::Up:
            return Make(Action::PreviousCandidate);
        case Key::PageDown:
        case Key::PageUp:
            // With the list open, a page at a time.
            if (!composer.IsCandidateListOpen()) break;
            return WithDelta(key.key == Key::PageDown ? Action::NextCandidate : Action::PreviousCandidate,
                             static_cast<int>(std::max<std::size_t>(options.pageSize, 1)));
        case Key::Left:
        case Key::Right:
            return WithDelta(key.shift ? Action::Resize : Action::MoveFocus, key.key == Key::Left ? -1 : 1);
        default:
            break;
        }
        // With the list open, 1-9 choose from the page shown.
        if (key.digit >= 1 && key.digit <= 9 && composer.IsCandidateListOpen() && !key.shift) {
            const std::size_t pageSize = std::max<std::size_t>(options.pageSize, 1);
            const std::size_t page = composer.FocusedSelection() / pageSize * pageSize;
            const auto* candidates = composer.FocusedCandidates();
            const std::size_t index = page + static_cast<std::size_t>(key.digit - 1);
            if (candidates && index < candidates->size()) {
                auto command = Make(Action::SelectCandidate);
                command.index = index;
                return command;
            }
        }
    }
    if (composing && !composer.IsConverted()) {
        // Before conversion the arrows edit the typed text, as in Microsoft IME.
        switch (key.key) {
        case Key::Left:
            return WithDelta(Action::MoveCaret, -1);
        case Key::Right:
            return WithDelta(Action::MoveCaret, 1);
        case Key::Home:
            return WithDelta(Action::MoveCaret, -100000);
        case Key::End:
            return WithDelta(Action::MoveCaret, 100000);
        case Key::Delete:
            return Make(Action::Delete);
        default:
            break;
        }
    }
    switch (key.key) {
    case Key::Space:
    case Key::Convert:
        if (composing) return Make(Action::Convert);
        // The caller finds the text (JapaneseComposer::Reconvert).
        if (key.key == Key::Convert) return Make(Action::Reconvert);
        if (key.key == Key::Space) {
            const bool fullWidth = options.fullWidthSpace != key.shift;
            // A half-width space is the application's own.
            if (!fullWidth) return std::nullopt;
            auto command = Make(Action::InsertOutside);
            command.character = L'\x3000';
            return command;
        }
        return std::nullopt;
    case Key::NonConvert:
        if (!composing) return std::nullopt;
        return Make(Action::CycleKana);
    case Key::Enter:
        if (!composing) return std::nullopt;
        return Make(Action::Commit);
    case Key::Backspace:
        if (!composing) return std::nullopt;
        return Make(Action::Backspace);
    case Key::Escape:
        if (!composing) return std::nullopt;
        return Make(Action::Cancel);
    case Key::F6:
    case Key::F7:
    case Key::F8:
    case Key::F9:
    case Key::F10: {
        if (!composing) return std::nullopt;
        constexpr KanaForm forms[] = {KanaForm::Hiragana, KanaForm::Katakana, KanaForm::HalfWidthKatakana,
                                      KanaForm::FullWidthAlphanumeric, KanaForm::HalfWidthAlphanumeric};
        auto command = Make(Action::Transliterate);
        command.form = forms[static_cast<int>(key.key) - static_cast<int>(Key::F6)];
        return command;
    }
    default:
        break;
    }
    if (IsCaretKey(key.key)) {
        if (!composing) return std::nullopt;
        auto command = Make(Action::Commit);
        command.letKeyThrough = true;
        return command;
    }
    if (key.key == Key::Character && key.character > 0x20 && key.character != 0x7F) {
        auto command = Make(Action::Insert);
        command.character = key.character;
        return command;
    }
    return std::nullopt;
}

KeyOutcome ApplyKey(JapaneseComposer& composer, const KeyCommand& command) {
    KeyOutcome outcome;
    switch (command.action) {
    case Action::Reconvert:
        break;
    case Action::ForgetChosen:
        composer.ForgetChosen();
        break;
    case Action::UndoCommit:
        if (const auto* text = composer.LastCommit()) {
            std::wstring committed = *text;
            if (composer.UndoCommit()) outcome.uncommitted = std::move(committed);
        }
        break;
    case Action::Insert:
        // After a conversion, typing on commits it and starts anew (live
        // conversion types on).
        if (composer.CommitsBeforeTyping()) outcome.committed = composer.Commit();
        composer.Insert(command.character);
        break;
    case Action::Backspace:
        composer.Backspace();
        break;
    case Action::Cancel:
        composer.Cancel();
        break;
    case Action::Convert:
        composer.Convert();
        break;
    case Action::CycleKana:
        composer.CycleKana();
        break;
    case Action::Transliterate:
        composer.Transliterate(command.form);
        break;
    case Action::NextCandidate:
        for (int i = 0; i < std::max(command.delta, 1); ++i) composer.NextCandidate();
        break;
    case Action::PreviousCandidate:
        for (int i = 0; i < std::max(command.delta, 1); ++i) composer.PreviousCandidate();
        break;
    case Action::MoveFocus:
        composer.MoveFocus(command.delta);
        break;
    case Action::Resize:
        composer.ResizeFocus(command.delta);
        break;
    case Action::SelectCandidate:
        composer.SelectCandidate(command.index);
        break;
    case Action::ChooseRow:
        if (composer.IsTyping()) {
            // A click on a prediction commits it.
            composer.ChoosePrediction(command.index);
            outcome.committed = composer.Commit();
        } else {
            composer.SelectCandidate(command.index);
        }
        break;
    case Action::NextPrediction:
        composer.NextPrediction();
        break;
    case Action::PreviousPrediction:
        composer.PreviousPrediction();
        break;
    case Action::ClearPrediction:
        composer.ClearPredictionChoice();
        break;
    case Action::MoveCaret:
        composer.MoveCaret(command.delta);
        break;
    case Action::Delete:
        composer.Delete();
        break;
    case Action::Commit:
        outcome.committed = composer.IsComposing() ? composer.Commit() : std::wstring{};
        break;
    case Action::InsertOutside:
        outcome.outside = std::wstring(1, command.character);
        break;
    }
    return outcome;
}

CandidateList CandidateListFor(const JapaneseComposer& composer, std::size_t pageSize) {
    pageSize = std::max<std::size_t>(pageSize, 1);
    CandidateList list;
    const auto& predictions = composer.Predictions();
    if (composer.IsTyping() && !predictions.empty()) {
        list.kind = CandidateList::Kind::Predictions;
        for (std::size_t i = 0; i < predictions.size(); ++i) {
            list.rows.push_back({predictions[i].text, predictions[i].reading, static_cast<std::uint32_t>(i + 1)});
        }
        list.selected = composer.ChosenPrediction();
        list.count = list.rows.size();
        return list;
    }
    const auto* candidates = composer.FocusedCandidates();
    if (!composer.IsCandidateListOpen() || !candidates || candidates->empty()) return list;
    list.kind = CandidateList::Kind::Candidates;
    const std::wstring reading = composer.FocusedReading();
    for (std::size_t i = 0; i < candidates->size(); ++i) {
        const auto& candidate = (*candidates)[i];
        list.rows.push_back({candidate.text, reading, static_cast<std::uint32_t>(i % pageSize + 1),
                             candidate.slip || candidate.spellingCorrection});
    }
    const std::size_t selected = std::min(composer.FocusedSelection(), candidates->size() - 1);
    list.selected = selected;
    list.pageStart = selected / pageSize * pageSize;
    list.count = std::min(pageSize, list.rows.size() - list.pageStart);
    return list;
}

}  // namespace tekito::japanese
