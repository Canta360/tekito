#include "Core/InputStateMachine.h"
#include "Core/ConversionEngine.h"

#include <algorithm>
#include <utility>

namespace tekito {

void InputStateMachine::BeginOrUpdate(std::wstring rawText, std::vector<Candidate> candidates) {
    if (inputMode_ == InputMode::Direct) {
        Reset();
        return;
    }

    rawText_ = std::move(rawText);
    candidates_ = std::move(candidates);

    if (rawText_.empty() || candidates_.empty()) {
        Reset();
        return;
    }

    state_ = CompositionState::Composing;
    selectedIndex_ = 0;
    pageStart_ = 0;
    visibleCount_ = std::min<std::size_t>(5, candidates_.size());
    candidateNavigationActive_ = false;
    boundarySpaceActive_ = false;
}

void InputStateMachine::SetInputMode(InputMode mode) noexcept {
    if (inputMode_ == mode) return;
    inputMode_ = mode;
    Reset();
}

InputAction InputStateMachine::OnNextCandidate() {
    if (!IsActive()) {
        return {};
    }

    if (state_ == CompositionState::Composing) {
        state_ = CompositionState::Cycling;
        selectedIndex_ = 0;
        candidateNavigationActive_ = true;
        boundarySpaceActive_ = false;
        UpdatePageForSelection();
        return ReplaceSelected();
    }

    selectedIndex_ = (selectedIndex_ + 1) % candidates_.size();
    candidateNavigationActive_ = true;
    UpdatePageForSelection(selectedIndex_ == 0);
    return ReplaceSelected();
}

InputAction InputStateMachine::OnSpace() {
    if (!IsActive()) return {};

    if (candidateNavigationActive_) {
        std::wstring text = candidates_[selectedIndex_].text;
        text.push_back(L' ');
        Reset();
        return {ActionKind::CommitAndStartNext, std::move(text), false};
    }

    if (state_ == CompositionState::Boundary) {
        if (candidates_.size() < 2) {
            std::wstring text = rawText_ + L"  ";
            Reset();
            return {ActionKind::CommitAndStartNext, std::move(text), false};
        }

        const auto raw = std::find_if(candidates_.begin(), candidates_.end(),
                                      [this](const Candidate& candidate) {
                                          return candidate.isOriginal || candidate.text == rawText_;
                                      });
        selectedIndex_ = raw == candidates_.end()
                             ? 0
                             : static_cast<std::size_t>(std::distance(candidates_.begin(), raw));
        selectedIndex_ = (selectedIndex_ + 1) % candidates_.size();
        state_ = CompositionState::Cycling;
        candidateNavigationActive_ = false;
        boundarySpaceActive_ = true;
        UpdatePageForSelection();
        return {ActionKind::ReplaceComposition, candidates_[selectedIndex_].text + L" ", true};
    }

    if (state_ == CompositionState::Cycling) {
        selectedIndex_ = (selectedIndex_ + 1) % candidates_.size();
        boundarySpaceActive_ = true;
        UpdatePageForSelection(selectedIndex_ == 0);
        return {ActionKind::ReplaceComposition, candidates_[selectedIndex_].text + L" ", true};
    }

    const auto selected = spaceBoundaryPolicy_.SelectCorrection(rawText_, candidates_);
    if (!selected) {
        std::wstring text = rawText_;
        text.push_back(L' ');
        state_ = CompositionState::Boundary;
        candidateNavigationActive_ = false;
        boundarySpaceActive_ = true;
        const auto raw = std::find_if(candidates_.begin(), candidates_.end(),
                                      [this](const Candidate& candidate) {
                                          return candidate.isOriginal || candidate.text == rawText_;
                                      });
        selectedIndex_ = raw == candidates_.end()
                             ? 0
                             : static_cast<std::size_t>(std::distance(candidates_.begin(), raw));
        UpdatePageForSelection();
        return {ActionKind::ReplaceComposition, std::move(text), false};
    }

    selectedIndex_ = *selected;
    state_ = CompositionState::Cycling;
    candidateNavigationActive_ = false;
    boundarySpaceActive_ = true;
    UpdatePageForSelection();
    return {ActionKind::ReplaceComposition, candidates_[selectedIndex_].text + L" ", true};
}

InputAction InputStateMachine::OnTab() {
    return OnNextCandidate();
}

InputAction InputStateMachine::OnPreviousCandidate() {
    if (!IsActive()) {
        return {};
    }

    state_ = CompositionState::Cycling;
    candidateNavigationActive_ = true;
    if (selectedIndex_ == 0) {
        selectedIndex_ = candidates_.size() - 1;
    } else {
        --selectedIndex_;
    }
    UpdatePageForSelection();
    return ReplaceSelected();
}

InputAction InputStateMachine::OnCandidateSelected(std::size_t index) {
    if (!IsActive() || index >= candidates_.size()) return {};

    state_ = CompositionState::Cycling;
    candidateNavigationActive_ = true;
    boundarySpaceActive_ = false;
    selectedIndex_ = index;
    UpdatePageForSelection();
    return ReplaceSelected();
}

InputAction InputStateMachine::OnShiftSpace() {
    return OnPreviousCandidate();
}

InputAction InputStateMachine::OnShiftTab() {
    return OnPreviousCandidate();
}

InputAction InputStateMachine::OnBackspace() {
    if (!IsActive()) {
        return {};
    }

    if (state_ == CompositionState::Boundary || state_ == CompositionState::Cycling) {
        state_ = CompositionState::Composing;
        selectedIndex_ = 0;
        pageStart_ = 0;
        visibleCount_ = std::min<std::size_t>(5, candidates_.size());
        candidateNavigationActive_ = false;
        boundarySpaceActive_ = false;
        return {ActionKind::RestoreOriginal, rawText_, true};
    }

    return {};
}

InputAction InputStateMachine::OnPrintable() {
    if (!IsActive()) {
        return {};
    }

    if (state_ == CompositionState::Boundary || state_ == CompositionState::Cycling) {
        Reset();
        return {ActionKind::CommitAndStartNext, {}, false};
    }

    return {};
}

InputAction InputStateMachine::OnPunctuation(wchar_t punctuation, PunctuationRole role) {
    if (!IsActive()) {
        return {};
    }

    const std::optional<std::size_t> explicitSelection =
        state_ == CompositionState::Cycling ? std::optional(selectedIndex_) : std::nullopt;
    const auto selected = autoApplyPolicy_.SelectForBoundary(rawText_, candidates_,
                                                              explicitSelection);
    std::wstring text = selected ? candidates_[*selected].text : rawText_;
    text.push_back(punctuation);
    Reset();
    return {ActionKind::ReplaceComposition, std::move(text), false, role};
}

InputAction InputStateMachine::OnCancel() {
    if (!IsActive()) {
        return {};
    }

    if (candidateNavigationActive_) {
        state_ = CompositionState::Composing;
        selectedIndex_ = 0;
        pageStart_ = 0;
        visibleCount_ = std::min<std::size_t>(5, candidates_.size());
        candidateNavigationActive_ = false;
        boundarySpaceActive_ = false;
        return {ActionKind::RestoreOriginal, rawText_, true};
    }

    std::wstring text = rawText_;
    Reset();
    return {ActionKind::ReplaceComposition, std::move(text), false};
}

InputAction InputStateMachine::OnEnter(bool addTerminalPeriod) {
    if (!IsActive()) {
        return {};
    }

    if (candidateNavigationActive_) {
        std::wstring text = candidates_[selectedIndex_].text;
        Reset();
        return {ActionKind::EndComposition, std::move(text), false};
    }

    const std::optional<std::size_t> explicitSelection =
        state_ == CompositionState::Cycling ? std::optional(selectedIndex_) : std::nullopt;
    const auto selected = autoApplyPolicy_.SelectForBoundary(rawText_, candidates_,
                                                              explicitSelection);
    std::wstring text = selected ? candidates_[*selected].text : rawText_;
    if (addTerminalPeriod && NeedsTerminalPeriod(text)) {
        text.push_back(L'.');
    }
    Reset();
    return {ActionKind::CommitBeforeNewline, std::move(text), false};
}

void InputStateMachine::Reset() noexcept {
    state_ = CompositionState::Idle;
    rawText_.clear();
    candidates_.clear();
    selectedIndex_ = 0;
    pageStart_ = 0;
    visibleCount_ = 0;
    candidateNavigationActive_ = false;
    boundarySpaceActive_ = false;
}

bool InputStateMachine::IsActive() const noexcept {
    return inputMode_ == InputMode::Convert && state_ != CompositionState::Idle &&
           !candidates_.empty();
}

bool InputStateMachine::IsCandidateNavigationActive() const noexcept {
    return IsActive() && candidateNavigationActive_;
}

InputMode InputStateMachine::Mode() const noexcept {
    return inputMode_;
}

CompositionState InputStateMachine::State() const noexcept {
    return state_;
}

const std::wstring& InputStateMachine::RawText() const noexcept {
    return rawText_;
}

const std::vector<Candidate>& InputStateMachine::Candidates() const noexcept {
    return candidates_;
}

std::size_t InputStateMachine::SelectedIndex() const noexcept {
    return selectedIndex_;
}

std::size_t InputStateMachine::PageStart() const noexcept {
    return pageStart_;
}

std::size_t InputStateMachine::VisibleCount() const noexcept {
    return visibleCount_;
}

InputAction InputStateMachine::ReplaceSelected() const {
    if (!IsActive() || selectedIndex_ >= candidates_.size()) {
        return {};
    }

    return {
        ActionKind::ReplaceComposition,
        boundarySpaceActive_ ? candidates_[selectedIndex_].text + L" "
                             : candidates_[selectedIndex_].text,
        true,
    };
}

void InputStateMachine::UpdatePageForSelection(bool resetOnLoop) noexcept {
    const auto count = candidates_.size();
    if (count == 0) {
        pageStart_ = 0;
        visibleCount_ = 0;
        return;
    }

    if (resetOnLoop || selectedIndex_ < 5) {
        pageStart_ = 0;
        visibleCount_ = std::min<std::size_t>(5, count);
        return;
    }

    if (selectedIndex_ < 10) {
        pageStart_ = 0;
        visibleCount_ = std::min<std::size_t>(10, count);
        return;
    }

    pageStart_ = (selectedIndex_ / 10) * 10;
    visibleCount_ = std::min<std::size_t>(10, count - pageStart_);
}

}  // namespace tekito
