#pragma once

#include "Core/AutoApplyPolicy.h"
#include "Core/Candidate.h"
#include "Core/InputMode.h"
#include "Core/SpaceBoundaryPolicy.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tekito {

enum class CompositionState {
    Idle,
    Composing,
    Boundary,
    Cycling,
};

enum class ActionKind {
    None,
    ReplaceComposition,
    RestoreOriginal,
    CommitAndStartNext,
    // The word is committed and the Enter key then goes to the application.
    CommitBeforeNewline,
    EndComposition,
};

enum class PunctuationRole {
    SentenceTerminal,
    ClauseSeparator,
};

struct InputAction {
    ActionKind kind{ActionKind::None};
    std::wstring text;
    bool showCandidates{false};
    PunctuationRole punctuationRole{PunctuationRole::ClauseSeparator};
};

class InputStateMachine final {
public:
    void BeginOrUpdate(std::wstring rawText, std::vector<Candidate> candidates);

    void SetInputMode(InputMode mode) noexcept;
    // Rows the list shows at first, and once the user pages through it
    // (UserSettings::candidateRows).
    void SetPageSizes(std::size_t first, std::size_t paged) noexcept;

    [[nodiscard]] InputAction OnNextCandidate();
    [[nodiscard]] InputAction OnPreviousCandidate();
    [[nodiscard]] InputAction OnCandidateSelected(std::size_t index);
    [[nodiscard]] InputAction OnSpace();
    [[nodiscard]] InputAction OnTab();
    [[nodiscard]] InputAction OnShiftSpace();
    [[nodiscard]] InputAction OnShiftTab();
    [[nodiscard]] InputAction OnBackspace();
    [[nodiscard]] InputAction OnPrintable();
    [[nodiscard]] InputAction OnPunctuation(wchar_t punctuation, PunctuationRole role);
    [[nodiscard]] InputAction OnCancel();
    // With addTerminalPeriod, a line that ends without punctuation gets a
    // period before the newline.
    [[nodiscard]] InputAction OnEnter(bool addTerminalPeriod = false);

    void Reset() noexcept;

    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] bool IsCandidateNavigationActive() const noexcept;
    [[nodiscard]] InputMode Mode() const noexcept;
    [[nodiscard]] CompositionState State() const noexcept;
    [[nodiscard]] const std::wstring& RawText() const noexcept;
    [[nodiscard]] const std::vector<Candidate>& Candidates() const noexcept;
    [[nodiscard]] std::size_t SelectedIndex() const noexcept;
    [[nodiscard]] std::size_t PageStart() const noexcept;
    [[nodiscard]] std::size_t VisibleCount() const noexcept;

private:
    [[nodiscard]] InputAction ReplaceSelected() const;
    void UpdatePageForSelection(bool resetOnLoop = false) noexcept;

    InputMode inputMode_{InputMode::Convert};
    CompositionState state_{CompositionState::Idle};
    std::wstring rawText_;
    std::vector<Candidate> candidates_;
    AutoApplyPolicy autoApplyPolicy_;
    SpaceBoundaryPolicy spaceBoundaryPolicy_;
    std::size_t selectedIndex_{0};
    std::size_t pageStart_{0};
    std::size_t visibleCount_{0};
    std::size_t firstPage_{5};
    std::size_t page_{10};
    bool candidateNavigationActive_{false};
    bool boundarySpaceActive_{false};
};

}  // namespace tekito
