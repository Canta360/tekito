#include "Core/BuiltinLexicon.h"
#include "Core/CandidateRanker.h"
#include "Core/CandidateEngine.h"
#include "Core/DataPackPath.h"
#include "Core/ExternalLexiconProvider.h"
#include "Core/ExternalSlangProvider.h"
#include "Core/ExternalRankingProviders.h"
#include "Core/MisspellingProvider.h"
#include "Core/LexiconProvider.h"
#include "Core/PolicyEngine.h"
#include "Core/SpaceBoundaryPolicy.h"
#include "Core/UserDictionary.h"
#include "Core/UserLexiconProvider.h"
#include "Core/UserLearning.h"
#include "Core/TypoModel.h"
#include "Dictionary/DictionaryService.h"
#include "Dictionary/ExternalDictionaryProvider.h"
#include "Core/InputStateMachine.h"
#include "UserData/SqliteUserDictionaryRepository.h"
#include "UserData/UserDictionaryFile.h"
#include "UserData/DataPackValidation.h"
#include "UserData/ImeDictionaryImport.h"
#include "UserData/PostalCodeImport.h"
#include "sqlite3.h"
#include "UserData/RuntimeModeState.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// The phrase pack is too large for the repository and is built locally
// (see docs/data-packs.md). Tests that need it are skipped when it is not
// installed; every other pack is required.
bool OptionalPackInstalled(const wchar_t* packId) {
    const bool installed = std::filesystem::exists(
        tekito::ExternalLexiconProvider::PackDirectory(packId) / L"manifest.json");
    if (!installed) {
        std::wcout << L"skipped: checks that need the " << packId << L" pack\n";
    }
    return installed;
}

bool IsOptionalPack(std::wstring_view packId) {
    return packId == L"phrase";
}

bool HasFlag(std::uint32_t value, std::uint32_t flag) {
    return (value & flag) == flag;
}

std::string NarrowAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(character >= 0 && character <= 0x7f ?
                             static_cast<char>(character) : '?');
    }
    return result;
}

std::vector<tekito::Candidate> MakeCandidates(std::size_t count) {
    std::vector<tekito::Candidate> result;
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back({L"candidate" + std::to_wstring(i + 1),
                          i == 2 ? tekito::SemanticLabel::Original : tekito::SemanticLabel::None,
                          i == 2});
    }
    return result;
}

void TestFirstSpaceAndLiveCycle() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"neme", MakeCandidates(4));

    auto action = state.OnTab();
    Require(action.kind == tekito::ActionKind::ReplaceComposition, "first Tab replaces composition");
    Require(action.text == L"candidate1", "first Tab selects candidate 1 without a boundary");
    Require(state.State() == tekito::CompositionState::Cycling, "first Tab enters Cycling");
    Require(state.IsCandidateNavigationActive(), "first Tab enters candidate navigation");

    action = state.OnTab();
    Require(action.text == L"candidate2", "second Tab selects candidate 2");

    action = state.OnTab();
    Require(action.text == L"candidate3", "third Tab reaches original candidate without skipping");
}

void TestSpaceBoundaryAndAutoApplyPolicy() {
    const std::vector<tekito::Candidate> correctionCandidates{
        {L"Hello", tekito::SemanticLabel::None, false, 99.0, false,
         tekito::CandidatePolicyCorrect,
         tekito::CandidateSourceGeneratedEditDistance | tekito::CandidateSourceSingleEdit},
        {L"Hallo", tekito::SemanticLabel::Original, true},
    };

    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"Hallo", correctionCandidates);
    const auto action = state.OnSpace();
    Require(action.text == L"Hello ", "high-confidence correction applies at Space");
    Require(action.showCandidates, "first Space keeps candidates visible for cycling");
    Require(state.IsActive(), "automatic correction remains reversible until next input");
    const auto undo = state.OnBackspace();
    Require(undo.kind == tekito::ActionKind::RestoreOriginal && undo.text == L"Hallo",
            "Backspace restores the raw word after automatic correction");

    const std::vector<tekito::Candidate> validWordCandidates{
        {L"Nice", tekito::SemanticLabel::Original, true, 100.0, false,
         tekito::CandidatePolicyNone, tekito::CandidateSourceEmbeddedLexicon},
        {L"Name", tekito::SemanticLabel::None, false, 98.0, false,
         tekito::CandidatePolicyCorrect, tekito::CandidateSourceGeneratedEditDistance},
    };
    state.BeginOrUpdate(L"Nice", validWordCandidates);
    const auto validWordBoundary = state.OnSpace();
    Require(validWordBoundary.kind == tekito::ActionKind::ReplaceComposition &&
                validWordBoundary.text == L"Nice " && state.State() == tekito::CompositionState::Boundary,
            "valid original word keeps a hidden boundary session after the first Space");
    const auto validWordConversion = state.OnSpace();
    Require(validWordConversion.text == L"Name " && validWordConversion.showCandidates &&
                state.State() == tekito::CompositionState::Cycling,
            "a second immediate Space explicitly enters conversion for a valid word");

    state.BeginOrUpdate(L"alias", {
        {L"target", tekito::SemanticLabel::Standard, false, 100.0, false,
         tekito::CandidatePolicyCorrect, tekito::CandidateSourceUserDictionary},
        {L"alias", tekito::SemanticLabel::Original, true},
    });
    Require(state.OnSpace().text == L"target " && state.IsActive(),
            "User Dictionary correct policy can be accepted at Space");

    state.BeginOrUpdate(L"protected", {
        {L"target", tekito::SemanticLabel::Standard, false, 100.0, false,
         tekito::CandidatePolicyProtect, tekito::CandidateSourceUserDictionary},
        {L"protected", tekito::SemanticLabel::Original, true},
    });
    const auto protectedUserBoundary = state.OnSpace();
    Require(protectedUserBoundary.kind == tekito::ActionKind::ReplaceComposition &&
                protectedUserBoundary.text == L"protected " && state.State() == tekito::CompositionState::Boundary,
            "User Dictionary protect policy keeps the raw word at the first Space");

    state.BeginOrUpdate(L"them", {
        {L"the", tekito::SemanticLabel::None, false, 100.0, false,
         tekito::CandidatePolicyCorrect, tekito::CandidateSourceGeneratedEditDistance},
        {L"them", tekito::SemanticLabel::Original, true, 90.0, false,
         tekito::CandidatePolicyProtect, tekito::CandidateSourceUserDictionary},
    });
    Require(state.OnSpace().text == L"them ",
            "Space never replaces a word the user keeps as typed, even behind a correction");

    state.BeginOrUpdate(L"Hallo", correctionCandidates);
    Require(state.OnPunctuation(L'.', tekito::PunctuationRole::SentenceTerminal).text == L"Hello.",
            "punctuation uses the same auto-apply policy");

    state.BeginOrUpdate(L"Hallo", correctionCandidates);
    Require(state.OnEnter().text == L"Hello",
            "Enter uses the auto-apply policy and adds nothing by default");
    state.BeginOrUpdate(L"Hallo", correctionCandidates);
    Require(state.OnEnter(true).text == L"Hello.",
            "Enter adds a terminal period when that option is on");

    const std::vector<tekito::Candidate> protectedCandidates{
        {L"Neme", tekito::SemanticLabel::ProperNoun, true, 100.0, true,
         tekito::CandidatePolicyProtect | tekito::CandidatePolicySuggestOnly,
         tekito::CandidateSourceProtectedPattern},
        {L"Name", tekito::SemanticLabel::None, false, 99.0, false,
         tekito::CandidatePolicyCorrect,
         tekito::CandidateSourceGeneratedEditDistance | tekito::CandidateSourceSingleEdit},
    };
    state.BeginOrUpdate(L"Neme", protectedCandidates);
    Require(state.OnSpace().text == L"Neme ",
            "protected proper nouns keep raw input at a boundary");
}

void TestSentenceBoundaryAndCapitalizationProvenance() {
    Require(tekito::IsSentenceStart(L""), "document start is a sentence start");
    Require(tekito::IsSentenceStart(L"Hello.  \""),
            "terminal punctuation before quote starts a sentence");
    Require(tekito::IsSentenceStart(L"Hello\r\n  "), "line start is a sentence start");
    Require(!tekito::IsSentenceStart(L"Hello, "), "clause separator is not a sentence start");
    Require(tekito::NeedsTerminalPeriod(L"Hello world "),
            "unterminated line needs a period");
    Require(!tekito::NeedsTerminalPeriod(L"Hello world!  "),
            "terminal punctuation is preserved");
    Require(!tekito::NeedsTerminalPeriod(L"Hello world,  "),
            "auto period does not create malformed double punctuation");
    Require(!tekito::NeedsTerminalPeriod(L"previous\r\n  "),
            "blank line does not receive a period");

    tekito::CandidateEngine engine;
    tekito::ConversionRequest automatic;
    automatic.rawText = L"Neme";
    automatic.context.sentenceStart = true;
    automatic.capitalization.origin = tekito::CapitalizationOrigin::EngineApplied;
    const auto automaticResult = engine.Convert(automatic);
    Require(!automaticResult.candidates.empty() && automaticResult.candidates[0].text == L"Name",
            "engine-applied sentence case does not protect a misspelling");

    tekito::ConversionRequest manual = automatic;
    manual.capitalization.origin = tekito::CapitalizationOrigin::UserTyped;
    const auto manualResult = engine.Convert(manual);
    Require(!manualResult.candidates.empty() && manualResult.candidates[0].text == L"Neme" &&
                manualResult.candidates[0].isProtected,
            "manual capitalization remains protected");
}

void TestSpaceCyclesAndPreservesCandidateSelection() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"he", {
        {L"hello", tekito::SemanticLabel::None, false, 80.0, false,
         tekito::CandidatePolicySuggestOnly, tekito::CandidateSourceCompletion},
        {L"he", tekito::SemanticLabel::Original, true},
        {L"the", tekito::SemanticLabel::None, false, 99.0, false,
         tekito::CandidatePolicyCorrect,
         tekito::CandidateSourceGeneratedEditDistance | tekito::CandidateSourceMissingCharacter},
    });

    auto action = state.OnSpace();
    Require(action.kind == tekito::ActionKind::ReplaceComposition && action.text == L"he " &&
                !action.showCandidates && state.State() == tekito::CompositionState::Boundary,
            "completion-only input uses a hidden normal Space boundary");

    state.BeginOrUpdate(L"neme", {
        {L"name", tekito::SemanticLabel::None, false, 99.0, false,
         tekito::CandidatePolicyCorrect,
         tekito::CandidateSourceGeneratedEditDistance | tekito::CandidateSourceSingleEdit},
        {L"neme", tekito::SemanticLabel::Original, true},
        {L"neume", tekito::SemanticLabel::None, false, 98.0, false,
         tekito::CandidatePolicyCorrect,
         tekito::CandidateSourceGeneratedEditDistance | tekito::CandidateSourceExtraCharacter},
    });
    action = state.OnSpace();
    Require(action.text == L"name " && action.showCandidates &&
                state.State() == tekito::CompositionState::Cycling,
            "correction candidate is accepted at the first Space boundary");
    action = state.OnSpace();
    Require(action.text == L"neme ", "second Space moves to the next candidate");
    action = state.OnPreviousCandidate();
    Require(action.text == L"name " && state.IsCandidateNavigationActive(),
            "Up after Space cycling keeps candidate navigation open with the boundary Space");
    action = state.OnNextCandidate();
    Require(action.text == L"neme ",
            "Down after Space cycling continues from the same candidate with the boundary Space");
    action = state.OnSpace();
    Require(action.text == L"neme ",
            "Space after arrow navigation confirms the selected candidate with one boundary");

    state.BeginOrUpdate(L"he", {
        {L"hello", tekito::SemanticLabel::None, false, 80.0, false,
         tekito::CandidatePolicySuggestOnly, tekito::CandidateSourceCompletion},
        {L"he", tekito::SemanticLabel::Original, true},
        {L"the", tekito::SemanticLabel::None, false, 99.0, false,
         tekito::CandidatePolicyCorrect,
         tekito::CandidateSourceGeneratedEditDistance | tekito::CandidateSourceMissingCharacter},
    });
    [[maybe_unused]] const auto firstTab = state.OnTab();
    [[maybe_unused]] const auto secondTab = state.OnTab();
    Require(state.SelectedIndex() == 1, "Tab selection updates the shared candidate index");
    Require(state.OnSpace().text == L"he ",
            "Space confirms the candidate selected by Tab without resetting to candidate one");
}

void TestEngineAndAutoApplyPolicyIntegration() {
    const tekito::BuiltinLexiconProvider builtinLexicon;
    const tekito::CandidateEngine engine(builtinLexicon);
    tekito::InputStateMachine state;

    const auto halloCandidates = engine.Generate(L"hallo");
    state.BeginOrUpdate(L"hallo", halloCandidates);
    Require(state.OnSpace().text == L"hello ",
            "engine metadata lets AutoApplyPolicy correct lowercase hallo");

    state.BeginOrUpdate(L"Nice", engine.Generate(L"Nice"));
    Require(state.OnSpace().text == L"Nice ",
            "engine metadata blocks ambiguous Nice correction");

    state.BeginOrUpdate(L"Neme", engine.Generate(L"Neme"));
    Require(state.OnSpace().text == L"Neme ",
            "engine metadata protects manually capitalized Neme");

    for (const auto& word : {std::wstring(L"Name"), std::wstring(L"Nice"),
                             std::wstring(L"meet")}) {
        state.BeginOrUpdate(word, engine.Generate(word));
        const auto boundary = state.OnSpace();
        Require(boundary.kind == tekito::ActionKind::ReplaceComposition &&
                    boundary.text == word + L" " &&
                    state.State() == tekito::CompositionState::Boundary,
                "valid lexical words use a hidden normal Space boundary");
    }

    const auto nemeCandidates = engine.Generate(L"neme");
    state.BeginOrUpdate(L"neme", nemeCandidates);
    const auto firstNemeSpace = state.OnSpace();
    Require(firstNemeSpace.text == L"name " && state.State() == tekito::CompositionState::Cycling,
            "lowercase neme accepts the ranked correction at a Space boundary");
    const auto firstNemeIndex = state.SelectedIndex();
    Require(firstNemeIndex < state.Candidates().size() &&
                state.Candidates()[firstNemeIndex].text + L" " == firstNemeSpace.text,
            "neme first Space text is produced by the selected Candidate");

    const auto secondNemeSpace = state.OnSpace();
    Require(state.SelectedIndex() == (firstNemeIndex + 1) % state.Candidates().size(),
            "neme second Space advances exactly one Candidate");
    Require(state.Candidates()[state.SelectedIndex()].text + L" " == secondNemeSpace.text,
            "neme second Space text remains sourced from the selected Candidate");

    state.BeginOrUpdate(L"neme", nemeCandidates);
    const auto rawPunctuation =
        state.OnPunctuation(L'.', tekito::PunctuationRole::SentenceTerminal);
    Require(rawPunctuation.text == L"neme.",
            "ambiguous punctuation keeps neme raw");

    state.BeginOrUpdate(L"neme", nemeCandidates);
    const auto rawEnter = state.OnEnter(true);
    Require(rawEnter.text == L"neme.",
            "ambiguous Enter keeps neme raw and adds a terminal period");

    state.BeginOrUpdate(L"neme", nemeCandidates);
    [[maybe_unused]] const auto accepted = state.OnSpace();
    const auto correctedPunctuation =
        state.OnPunctuation(L'.', tekito::PunctuationRole::SentenceTerminal);
    Require(correctedPunctuation.text == L"name.",
            "punctuation after Space uses the currently selected Candidate");
}

void TestFixedPageSize() {
    tekito::InputStateMachine state;
    state.SetPageSizes(7, 7);
    std::vector<tekito::Candidate> candidates;
    for (int i = 0; i < 20; ++i) candidates.push_back({L"word" + std::to_wstring(i)});
    state.BeginOrUpdate(L"wrod", candidates);
    Require(state.VisibleCount() == 7, "a fixed row count shows that many at first");
    for (int i = 0; i < 8; ++i) (void)state.OnNextCandidate();
    Require(state.PageStart() == 7 && state.VisibleCount() == 7, "and pages by the same count");
}

void TestPagingAndLoop() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"x", MakeCandidates(23));
    [[maybe_unused]] auto firstPageAction = state.OnTab();
    Require(state.VisibleCount() == 5, "initial page has 5 candidates");

    for (int i = 0; i < 5; ++i) {
        [[maybe_unused]] auto ignored = state.OnTab();
    }
    Require(state.SelectedIndex() == 5, "selection reaches candidate 6");
    Require(state.PageStart() == 0 && state.VisibleCount() == 10,
            "candidate 6 expands the first page to 10");

    for (int i = 0; i < 5; ++i) {
        [[maybe_unused]] auto ignored = state.OnTab();
    }
    Require(state.SelectedIndex() == 10, "selection reaches candidate 11");
    Require(state.PageStart() == 10 && state.VisibleCount() == 10,
            "candidate 11 opens the second page");

    while (state.SelectedIndex() != 22) {
        [[maybe_unused]] auto ignored = state.OnTab();
    }
    const auto loopAction = state.OnTab();
    Require(state.SelectedIndex() == 0, "last candidate loops to candidate 1");
    Require(state.PageStart() == 0 && state.VisibleCount() == 5,
            "loop resets display to first 5 candidates");
    Require(loopAction.text == L"candidate1", "loop replaces text with candidate 1");
}

void TestShiftSpaceAndRestore() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(12));
    auto action = state.OnShiftSpace();
    Require(state.SelectedIndex() == 11, "Shift+Space from start reaches last candidate");
    Require(action.text == L"candidate12", "Shift+Space replaces with last candidate");

    action = state.OnBackspace();
    Require(action.kind == tekito::ActionKind::RestoreOriginal, "Backspace restores original");
    Require(action.text == L"raw", "Backspace uses exact raw input");
    Require(state.State() == tekito::CompositionState::Composing, "Backspace returns to composing");
}

void TestTabAndShiftTabCandidateSelection() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(4));

    auto action = state.OnTab();
    Require(action.kind == tekito::ActionKind::ReplaceComposition, "Tab replaces composition");
    Require(action.text == L"candidate1", "Tab selects candidate 1");

    action = state.OnTab();
    Require(action.text == L"candidate2", "second Tab selects candidate 2");

    action = state.OnShiftTab();
    Require(action.text == L"candidate1", "Shift+Tab returns to candidate 1");

    action = state.OnSpace();
    Require(action.kind == tekito::ActionKind::CommitAndStartNext &&
                action.text == L"candidate1 " && !state.IsActive(),
            "Space confirms the current navigation candidate with one word boundary");
}

void TestCandidateClickAndArrowNavigationState() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(23));
    Require(!state.IsCandidateNavigationActive(),
            "normal composition does not start in candidate navigation");

    auto action = state.OnCandidateSelected(10);
    Require(action.text == L"candidate11" && state.IsCandidateNavigationActive(),
            "candidate click enters navigation at the clicked candidate");
    Require(state.SelectedIndex() == 10 && state.PageStart() == 10 &&
                state.VisibleCount() == 10,
            "clicked candidate and paging share the same selected index");

    action = state.OnNextCandidate();
    Require(action.text == L"candidate12" && state.SelectedIndex() == 11,
            "Down or Tab continues after the clicked candidate");
    action = state.OnPreviousCandidate();
    Require(action.text == L"candidate11" && state.SelectedIndex() == 10,
            "Up or Shift+Tab returns to the previous candidate");

    action = state.OnEnter();
    Require(action.kind == tekito::ActionKind::EndComposition &&
                action.text == L"candidate11" && !state.IsActive(),
            "Enter confirms the current clicked candidate without a newline");
}

void TestSemanticCandidateActions() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(3));

    auto action = state.OnNextCandidate();
    Require(action.text == L"candidate1",
            "semantic next-candidate action selects candidate 1");
    action = state.OnPreviousCandidate();
    Require(action.text == L"candidate3",
            "semantic previous-candidate action loops to the last candidate");
}

void TestInputModeDirectPassThrough() {
    tekito::InputStateMachine state;
    Require(state.Mode() == tekito::InputMode::Convert,
            "input core defaults to Convert mode");

    state.SetInputMode(tekito::InputMode::Direct);
    Require(state.Mode() == tekito::InputMode::Direct,
            "input core can enter Direct mode");
    state.BeginOrUpdate(L"raw", MakeCandidates(3));
    Require(!state.IsActive(), "Direct mode does not start a composition");
    Require(state.OnNextCandidate().kind == tekito::ActionKind::None,
            "Direct mode does not handle candidate navigation");

    state.SetInputMode(tekito::InputMode::Convert);
    state.BeginOrUpdate(L"raw", MakeCandidates(3));
    Require(state.IsActive(), "Convert mode restores composition handling");
}

void TestEnterCandidateNavigation() {
    tekito::InputStateMachine composing;
    composing.BeginOrUpdate(L"hello", MakeCandidates(3));
    auto action = composing.OnEnter();
    Require(action.kind == tekito::ActionKind::CommitBeforeNewline,
            "composing Enter commits and lets the newline through");
    Require(action.text == L"hello", "composing Enter commits the raw input as typed");
    Require(!composing.IsActive(), "composing Enter closes candidate session");

    tekito::InputStateMachine cycling;
    cycling.BeginOrUpdate(L"hello", MakeCandidates(3));
    [[maybe_unused]] auto selected = cycling.OnTab();
    action = cycling.OnEnter();
    Require(action.kind == tekito::ActionKind::EndComposition,
            "candidate-navigation Enter commits without inserting newline");
    Require(action.text == L"candidate1", "candidate-navigation Enter commits selected candidate only");
    Require(!cycling.IsActive(), "cycling Enter closes candidate session");
}


void TestNoSkippedCandidatesAcrossFullCycle() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(37));

    for (std::size_t expected = 0; expected < 37; ++expected) {
        const auto action = state.OnTab();
        Require(state.SelectedIndex() == expected, "Space must visit every candidate in order");
        Require(action.text == L"candidate" + std::to_wstring(expected + 1),
                "live replacement must match selected candidate");
    }

    const auto loop = state.OnTab();
    Require(state.SelectedIndex() == 0, "full cycle loops to first candidate");
    Require(loop.text == L"candidate1", "loop text is candidate 1");
}

void TestPrintableAfterCyclingStartsCleanNextSession() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"old", MakeCandidates(3));
    [[maybe_unused]] auto selected = state.OnTab();
    const auto action = state.OnPrintable();
    Require(action.kind == tekito::ActionKind::CommitAndStartNext,
            "printable key after cycling commits previous candidate");
    Require(!state.IsActive(), "previous candidate session is reset before next word");
}

void TestPunctuationCommitsCurrentCandidateWithoutSpace() {
    for (const wchar_t punctuation : {L'.', L'!', L'?'}) {
        tekito::InputStateMachine composing;
        composing.BeginOrUpdate(L"neme", MakeCandidates(3));
        const auto rawPunctuation =
            composing.OnPunctuation(punctuation, tekito::PunctuationRole::SentenceTerminal);
        Require(rawPunctuation.kind == tekito::ActionKind::ReplaceComposition,
                "composing sentence punctuation replaces current composition");
        Require(rawPunctuation.text == std::wstring(L"neme") + punctuation,
                "composing sentence punctuation preserves raw input");
        Require(rawPunctuation.punctuationRole == tekito::PunctuationRole::SentenceTerminal,
                "sentence punctuation keeps its role");
        Require(!composing.IsActive(), "sentence punctuation closes candidate session");
    }

    for (const wchar_t punctuation : {L',', L':', L';'}) {
        tekito::InputStateMachine composing;
        composing.BeginOrUpdate(L"neme", MakeCandidates(3));
        const auto rawPunctuation =
            composing.OnPunctuation(punctuation, tekito::PunctuationRole::ClauseSeparator);
        Require(rawPunctuation.kind == tekito::ActionKind::ReplaceComposition,
                "composing separator punctuation replaces current composition");
        Require(rawPunctuation.text == std::wstring(L"neme") + punctuation,
                "composing separator punctuation preserves raw input");
        Require(rawPunctuation.punctuationRole == tekito::PunctuationRole::ClauseSeparator,
                "separator punctuation keeps its role");
        Require(!composing.IsActive(), "separator punctuation closes candidate session");
    }

    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(3));
    [[maybe_unused]] auto selected = state.OnTab();
    const auto action = state.OnPunctuation(L'.', tekito::PunctuationRole::SentenceTerminal);
    Require(action.kind == tekito::ActionKind::ReplaceComposition,
            "punctuation replaces current composition");
    Require(action.text == L"candidate1.", "punctuation commits selected candidate without space");
    Require(action.punctuationRole == tekito::PunctuationRole::SentenceTerminal,
            "sentence punctuation keeps its role");
    Require(!state.IsActive(), "punctuation closes candidate session");

    state.BeginOrUpdate(L"raw", MakeCandidates(3));
    selected = state.OnTab();
    const auto separator = state.OnPunctuation(L',', tekito::PunctuationRole::ClauseSeparator);
    Require(separator.text == L"candidate1,", "separator commits without space");
    Require(separator.punctuationRole == tekito::PunctuationRole::ClauseSeparator,
            "separator punctuation keeps its role");
}

void TestCancelRestoresRawInput() {
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"raw", MakeCandidates(3));
    [[maybe_unused]] auto selected = state.OnTab();
    const auto action = state.OnCancel();
    Require(action.kind == tekito::ActionKind::RestoreOriginal,
            "candidate-navigation Esc restores current composition");
    Require(action.text == L"raw", "cancel restores exact raw input");
    Require(state.IsActive() && !state.IsCandidateNavigationActive(),
            "candidate-navigation Esc returns to normal composition");

    const auto secondCancel = state.OnCancel();
    Require(secondCancel.kind == tekito::ActionKind::ReplaceComposition && !state.IsActive(),
            "Esc outside candidate navigation closes the composition");
}

void TestConversionEngineBoundary() {
    const auto engine = tekito::CreateDefaultConversionEngine();
    Require(engine != nullptr, "default conversion engine is available");
    const auto result = engine->Convert({L"Neme"});
    Require(!result.candidates.empty(), "conversion result returns candidates");
    Require(result.candidates.front().text == L"Neme",
            "conversion boundary preserves the Phase 0 original candidate");
}

void TestCandidateEngine() {
    const tekito::BuiltinLexiconProvider builtinLexicon;
    tekito::CandidateEngine engine(builtinLexicon);
    const auto candidates = engine.Generate(L"Neme");
    Require(candidates.size() >= 5, "demo engine produces enough candidates");
    Require(candidates[0].id == 1 && candidates[1].id == 2, "candidate ids follow display order");
    Require(candidates[0].replaceSpan.start == 0 && candidates[0].replaceSpan.length == 4,
            "candidate replace span covers current input block");
    Require(candidates[0].text == L"Neme" && candidates[0].isOriginal,
            "capitalized unknown token is protected as original");
    Require(candidates[0].isProtected &&
                HasFlag(candidates[0].policyFlags, tekito::CandidatePolicyProtect),
            "protected original carries protect policy");
    Require(candidates[1].text == L"Name", "Name remains selectable after protected original");
    Require(candidates[1].dictionaryEntryId == L"embedded:name",
            "generated correction points at embedded dictionary entry");

    const auto lowerCandidates = engine.Generate(L"neme");
    Require(lowerCandidates[0].text == L"name", "lowercase neme stays lowercase");
    Require(lowerCandidates[1].text == L"neme" && lowerCandidates[1].isOriginal,
            "generated correction keeps original candidate near the top");

    tekito::ConversionRequest todayRequest;
    todayRequest.rawText = L"today";
    const auto today = engine.Convert(todayRequest).candidates;
    const auto firstDate = std::find_if(today.begin(), today.end(), [](const auto& candidate) {
        return candidate.text.find(L"-") != std::wstring::npos;  // 2026-09-29
    });
    Require(firstDate != today.end() && firstDate != today.begin() && today.front().text == L"today",
            "today offers the date after the word as typed");
    Require(tekito::SpaceBoundaryPolicy{}.SelectCorrection(L"today", today) != std::optional<std::size_t>(
                static_cast<std::size_t>(firstDate - today.begin())),
            "a date is never put in by Space");
    todayRequest.options.special.dates = false;
    const auto plainToday = engine.Convert(todayRequest).candidates;
    Require(std::none_of(plainToday.begin(), plainToday.end(),
                         [](const auto& candidate) { return candidate.text.find(L"-") != std::wstring::npos; }),
            "with dates off, today is only the word");

    const auto heCandidates = engine.Generate(L"he");
    Require(heCandidates.size() > 1 && heCandidates.front().text == L"he" &&
                heCandidates.front().isOriginal,
            "valid he remains the first original while suggestions stay available");
    Require(std::none_of(heCandidates.begin(), heCandidates.end(), [](const auto& candidate) {
                return !candidate.isOriginal &&
                       HasFlag(candidate.policyFlags, tekito::CandidatePolicyCorrect);
            }),
            "valid he suggestions are not promoted to automatic corrections without context");
    Require(HasFlag(lowerCandidates[0].policyFlags, tekito::CandidatePolicyCorrect) &&
                HasFlag(lowerCandidates[0].sourceFlags, tekito::CandidateSourceGeneratedEditDistance),
            "lowercase neme is a generated correction candidate");

    const auto halloCandidates = engine.Generate(L"Hallo");
    Require(std::any_of(halloCandidates.begin(), halloCandidates.end(),
                        [](const auto& candidate) { return candidate.text == L"Hello"; }),
            "one-edit correction keeps Hallo -> Hello");

    const auto niceCandidates = engine.Generate(L"Nice");
    Require(std::none_of(niceCandidates.begin(), niceCandidates.end(),
                         [](const auto& candidate) { return candidate.text == L"Name"; }),
             "two generic substitutions do not turn Nice into Name");

    const auto acronymCandidates = engine.Generate(L"NASA");
    Require(acronymCandidates[0].text == L"NASA" && acronymCandidates[0].isOriginal,
            "all caps token is protected");

    const auto mixedCandidates = engine.Generate(L"iPhone");
    Require(mixedCandidates[0].text == L"iPhone" && mixedCandidates[0].isOriginal,
            "mixed case token is protected");

    const auto codeLikeCandidates = engine.Generate(L"ESP32-C6");
    Require(codeLikeCandidates[0].text == L"ESP32-C6" && codeLikeCandidates[0].isOriginal,
            "code-like token is protected");

    const auto iCandidates = engine.Generate(L"i");
    Require(iCandidates[0].text == L"I", "i normalizes to I");
    Require(iCandidates[0].dictionaryEntryId == L"embedded:i",
            "normalization candidate points at embedded dictionary entry");
    Require(HasFlag(iCandidates[0].policyFlags, tekito::CandidatePolicyNormalize),
            "i correction is marked as normalization");
    Require(iCandidates[1].isOriginal, "i original remains selectable");

    const auto imCandidates = engine.Generate(L"im");
    Require(imCandidates[0].text == L"I'm", "im normalizes to I'm");

    const auto iveCandidates = engine.Generate(L"ive");
    Require(iveCandidates[0].text == L"I've", "ive normalizes to I've");
    Require(iveCandidates[1].text == L"ive" && iveCandidates[1].isOriginal,
            "ive original remains selectable");

    const auto transposedCandidates = engine.Generate(L"hte");
    Require(transposedCandidates[0].text == L"the", "transposed letters correct through edit distance");
    Require(transposedCandidates[0].score == 99.0, "transposition counts as one edit");
    Require(HasFlag(transposedCandidates[0].sourceFlags, tekito::CandidateSourceTransposition),
            "transposition correction carries source flag");
    Require(transposedCandidates[1].text == L"hte" && transposedCandidates[1].isOriginal,
            "transposition keeps original candidate near the top");

    const auto repeatedKeyCandidates = engine.Generate(L"meeet");
    Require(repeatedKeyCandidates[0].text == L"meet", "repeated key typo corrects to lexicon word");
    Require(HasFlag(repeatedKeyCandidates[0].sourceFlags, tekito::CandidateSourceRepeatedKey),
            "repeated key correction carries source flag");
    Require(repeatedKeyCandidates[1].text == L"meeet" && repeatedKeyCandidates[1].isOriginal,
            "repeated key correction keeps original candidate near the top");

    const auto missingCharacterCandidates = engine.Generate(L"thnk");
    Require(missingCharacterCandidates[0].text == L"think", "missing character typo corrects to lexicon word");
    Require(HasFlag(missingCharacterCandidates[0].sourceFlags, tekito::CandidateSourceMissingCharacter),
            "missing character correction carries source flag");
    Require(missingCharacterCandidates[1].text == L"thnk" && missingCharacterCandidates[1].isOriginal,
            "missing character correction keeps original candidate near the top");

    const auto extraCharacterCandidates = engine.Generate(L"becausee");
    Require(extraCharacterCandidates[0].text == L"because", "extra character typo corrects to lexicon word");
    Require(HasFlag(extraCharacterCandidates[0].sourceFlags, tekito::CandidateSourceExtraCharacter),
            "extra character correction carries source flag");
    Require(extraCharacterCandidates[1].text == L"becausee" && extraCharacterCandidates[1].isOriginal,
            "extra character correction keeps original candidate near the top");

    const auto prefixCandidates = engine.Generate(L"n");
    Require(prefixCandidates[0].text == L"name", "one-letter prefix exposes lexicon completion");
    Require(prefixCandidates[1].text == L"n" && prefixCandidates[1].isOriginal,
            "prefix completion keeps original candidate near the top");
    Require(HasFlag(prefixCandidates[0].policyFlags, tekito::CandidatePolicySuggestOnly),
            "prefix completion is suggest-only");
    Require(HasFlag(prefixCandidates[0].sourceFlags, tekito::CandidateSourceCompletion),
            "prefix completion carries source flag");

    const auto susCandidates = engine.Generate(L"sus");
    Require(susCandidates[0].text == L"sus" && susCandidates[0].isOriginal,
            "slang remains original candidate 1");
    Require(susCandidates[0].dictionaryEntryId == L"tekito-owned:sus",
            "slang original points at TEKITO-owned entry");
    Require(susCandidates[0].isProtected &&
                HasFlag(susCandidates[0].sourceFlags, tekito::CandidateSourceTekitoOwnedSlang),
            "slang original is TEKITO-owned protected data");
    Require(susCandidates.size() > 1 && susCandidates[1].text == L"suspicious",
            "slang standard form remains optional");
    Require(HasFlag(susCandidates[1].policyFlags, tekito::CandidatePolicyExpand),
            "slang standard form is an expansion candidate");

    const auto lolCandidates = engine.Generate(L"lol");
    Require(lolCandidates[0].text == L"lol" && lolCandidates[0].isOriginal,
            "lol remains original candidate 1");
    Require(lolCandidates.size() > 1 && lolCandidates[1].text == L"laughing out loud",
            "lol expansion remains optional");

    const auto nocapCandidates = engine.Generate(L"nocap");
    Require(nocapCandidates[0].text == L"nocap" && nocapCandidates[0].isOriginal,
            "nocap remains original candidate 1");
    Require(nocapCandidates.size() > 1 && nocapCandidates[1].text == L"no cap",
            "nocap standard form remains optional");

    const auto idkCandidates = engine.Generate(L"idk");
    Require(idkCandidates[0].text == L"idk" && idkCandidates[0].isOriginal,
            "idk remains original candidate 1");
    Require(idkCandidates.size() > 1 && idkCandidates[1].text == L"I don't know",
            "idk expansion remains optional");

    const auto happyCandidates = engine.Generate(L"Happy");
    Require(happyCandidates[0].text == L"Happy" && happyCandidates[0].isOriginal,
            "correct emotion word remains candidate 1");
    Require(happyCandidates.size() > 1 && happyCandidates[1].label == tekito::SemanticLabel::Emoji,
            "emotion word exposes emoji as optional candidate");
    Require(happyCandidates[1].dictionaryEntryId == L"emoji:happy",
            "emoji candidate points at emoji entry");
    Require(HasFlag(happyCandidates[1].policyFlags, tekito::CandidatePolicyEmoji) &&
                HasFlag(happyCandidates[1].sourceFlags, tekito::CandidateSourceEmoji),
            "emoji candidate carries emoji policy and source");
}

void TestBuiltinLexiconData() {
    std::unordered_set<std::wstring> rawEntries;

    for (const auto& entry : tekito::BuiltinLexicon()) {
        Require(!entry.raw.empty(), "builtin lexicon raw text is not empty");
        Require(rawEntries.insert(std::wstring(entry.raw)).second,
                "builtin lexicon raw entries are unique");

        for (const wchar_t ch : entry.raw) {
            Require(!std::iswalpha(ch) || !std::iswupper(ch),
                    "builtin lexicon raw text is lowercase-normalized");
        }
    }
}

void TestBuiltinLexiconProvider() {
    const tekito::BuiltinLexiconProvider provider;
    const auto entries = provider.Entries();
    Require(entries.size() == tekito::BuiltinLexicon().size(),
            "builtin provider exposes the existing lexicon data");
    Require(!entries.empty(), "builtin provider exposes at least one entry");

    tekito::CandidateGenerator generator(provider);
    const auto candidates = generator.Generate(L"Neme");
    Require(!candidates.empty() && candidates.front().text == L"Neme",
            "candidate generator can consume an injected lexicon provider");

    tekito::CandidateEngine engine(provider);
    Require(engine.Generate(L"Neme").front().text == L"Neme",
            "conversion engine keeps the generator compatibility wrapper");
}

void TestExternalLexiconProvider() {
    const auto path = std::filesystem::temp_directory_path() /
                      L"tekito-external-lexicon-test.txt";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "# generated ESDB word-list\n"
               << "60 A: example <n>\n"
               << "he\n"
               << "Hello\n"
               << "hello\n"
               << "not-a-word!\n"
               << "there\tmetadata\n"
               ;
    }

    Require(tekito::ExternalLexiconProvider::BuildIndex(path),
            "external lexicon builds its local search index");
    const tekito::ExternalLexiconProvider provider(path);
    Require(provider.IsLoaded(), "external lexicon reports an indexed data pack");
    Require(tekito::ExternalLexiconProvider::DefaultPath().is_absolute(),
            "external lexicon default path is absolute");
    Require(provider.IndexEntryCount() > 0 && provider.DataPath().is_absolute() &&
                provider.DataIndexPath().is_absolute(),
            "external lexicon exposes indexed diagnostics without materializing the word list");
    std::vector<std::wstring> words;
    bool hasExternalSource = false;
    provider.Visit([&](const auto& entry) {
        words.emplace_back(entry.raw);
        hasExternalSource = hasExternalSource ||
                            HasFlag(entry.sourceFlags,
                                    tekito::CandidateSourceExternalLexicon);
    });
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    Require(provider.Entries().empty(),
            "external lexicon does not materialize the full pack in Entries");
    Require(words.size() == 4,
            "external lexicon normalizes and de-duplicates word-list rows");
    Require(std::find(words.begin(), words.end(), L"he") != words.end(),
            "external lexicon normalizes words to lowercase");
    Require(hasExternalSource, "external lexicon preserves source metadata");

    std::vector<std::wstring> exactWords;
    provider.Find({tekito::LexiconQuery::Kind::Exact, L"HELLO"}, [&](const auto& entry) {
        exactWords.emplace_back(entry.raw);
    });
    Require(exactWords.size() == 2,
            "external exact query uses the indexed word range");

    std::vector<std::wstring> prefixWords;
    provider.Find({tekito::LexiconQuery::Kind::Prefix, L"he"}, [&](const auto& entry) {
        prefixWords.emplace_back(entry.raw);
    });
    Require(std::find(prefixWords.begin(), prefixWords.end(), L"he") != prefixWords.end() &&
                std::find(prefixWords.begin(), prefixWords.end(), L"hello") != prefixWords.end(),
            "external prefix query returns only matching indexed rows");

    const tekito::CandidateGenerator generator(provider);
    const auto candidates = generator.Generate(L"helo");
    Require(std::any_of(candidates.begin(), candidates.end(),
                        [](const auto& candidate) {
                            return candidate.text == L"helo" && candidate.isOriginal;
                        }),
            "candidate generator can consume an external lexicon");
    Require(std::any_of(candidates.begin(), candidates.end(),
                        [](const auto& candidate) {
                            return candidate.text == L"hello" &&
                                   HasFlag(candidate.sourceFlags,
                                           tekito::CandidateSourceExternalLexicon);
                        }),
            "external lexicon correction carries its source metadata");

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(tekito::ExternalLexiconProvider::IndexPath(path), error);
}

void TestWikipediaCommonMisspellingsProvider() {
    const auto path = std::filesystem::temp_directory_path() /
                      L"tekito-wikipedia-common-misspellings-test.tsv";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "definately\tdefinitely\n"
               << "occured\toccurred\n"
               << "seperate\tseparate\n";
    }

    Require(tekito::WikipediaCommonMisspellingsProvider::BuildIndex(path),
            "Wikipedia misspelling pack builds its local index");
    const tekito::WikipediaCommonMisspellingsProvider provider(path);
    Require(provider.IsLoaded(), "Wikipedia misspelling pack loads its indexed data");
    Require(tekito::WikipediaCommonMisspellingsProvider::DefaultPath().is_absolute(),
            "Wikipedia misspelling default path is absolute");

    std::vector<std::wstring> corrections;
    provider.Find(L"DEFINATELY", [&](const auto& entry) {
        corrections.emplace_back(entry.correction);
    });
    Require(corrections.size() == 1 && corrections.front() == L"definitely",
            "Wikipedia provider performs case-insensitive indexed lookup");
    Require(provider.IndexPath(path).extension() == L".idx",
            "Wikipedia provider uses a sidecar index");

    const tekito::BuiltinLexiconProvider lexicon;
    const tekito::CandidateGenerator generator(lexicon, provider);
    for (const auto& pair : {std::pair{L"definately", L"definitely"},
                             std::pair{L"seperate", L"separate"},
                             std::pair{L"occured", L"occurred"}}) {
        const auto candidates = generator.Generate(pair.first);
        const auto it = std::find_if(candidates.begin(), candidates.end(),
                                     [&](const auto& candidate) {
                                         return candidate.text == pair.second;
                                     });
        Require(it != candidates.end(),
                "Wikipedia-derived spelling correction is generated as a candidate");
        Require(HasFlag(it->sourceFlags,
                        tekito::CandidateSourceWikipediaMisspelling),
                "Wikipedia correction keeps its independent source metadata");
        Require(HasFlag(it->policyFlags, tekito::CandidatePolicyCorrect),
                "Wikipedia correction uses the normal correction policy");
    }

    const auto correct = generator.Generate(L"definitely");
    Require(std::none_of(correct.begin(), correct.end(), [](const auto& candidate) {
                return HasFlag(candidate.sourceFlags,
                               tekito::CandidateSourceWikipediaMisspelling);
            }),
            "a correct word does not receive a Wikipedia misspelling mapping");

    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(tekito::WikipediaCommonMisspellingsProvider::IndexPath(path), error);
}

void TestInstalledDataPacks() {
    Require(std::filesystem::exists(tekito::ExternalLexiconProvider::DefaultPath()),
            "installed Standard English Data Pack file exists at the default path");
    Require(std::filesystem::exists(tekito::ExternalLexiconProvider::IndexPath(
                tekito::ExternalLexiconProvider::DefaultPath())),
            "installed Standard English Data Pack index exists at the default path");
    const tekito::ExternalLexiconProvider lexicon(
        tekito::ExternalLexiconProvider::DefaultPath());
    Require(lexicon.IsLoaded(),
            "installed Standard English Data Pack loads through the default path");
    for (const auto& word : {L"world", L"tomorrow", L"yesterday", L"definitely",
                             L"received", L"message", L"separate", L"address",
                             L"anthropomorphization", L"archimedean", L"cellpadding",
                             L"compatibilities", L"correctors", L"differentiations",
                             L"disputandum", L"drumless", L"endoliths", L"extremophile",
                             L"futhark", L"futhorc", L"geometers", L"hydrophile",
                             L"hydrophobe", L"interpretor", L"johannine", L"mccarthyist",
                             L"milieux", L"millennialism", L"octahedra", L"papanicolaou",
                             L"parallelly", L"premillennial", L"premonstratensians",
                             L"prolegomena", L"repartition", L"resignment",
                             L"unmaneuverable", L"unmanoeuvrable"}) {
        bool found = false;
        lexicon.Find({tekito::LexiconQuery::Kind::Exact, word},
                     [&](const auto&) { found = true; });
        Require(found, "Standard English Data Pack serves required words through its index");
    }

    const tekito::WikipediaCommonMisspellingsProvider wikipedia(
        tekito::WikipediaCommonMisspellingsProvider::DefaultPath());
    Require(wikipedia.IsLoaded(),
            "installed Wikipedia correction Data Pack loads through the default path");
    std::vector<std::wstring> apenninesCorrections;
    wikipedia.Find(L"apenines", [&](const auto& entry) {
        apenninesCorrections.emplace_back(entry.correction);
    });
    Require(apenninesCorrections.size() == 1 && apenninesCorrections.front() == L"Apennines",
            "Wikipedia correction index deduplicates case-only source rows");
    std::vector<std::wstring> semanticCorrections;
    wikipedia.Find(L"muhammadan", [&](const auto& entry) {
        semanticCorrections.emplace_back(entry.correction);
    });
    Require(semanticCorrections.empty(),
            "Wikipedia correction pack excludes semantic replacement rows");
    const tekito::CandidateEngine engine;
    for (const auto& pair : {std::pair{L"definately", L"definitely"},
                             std::pair{L"recieved", L"received"},
                             std::pair{L"seperate", L"separate"},
                             std::pair{L"adress", L"address"}}) {
        const auto candidates = engine.Generate(pair.first);
        Require(std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                    return candidate.text == pair.second &&
                           HasFlag(candidate.sourceFlags,
                                   tekito::CandidateSourceWikipediaMisspelling);
                }),
                "default conversion engine uses the installed Wikipedia Provider");
    }

    const tekito::ExternalSlangProvider social(
        tekito::ExternalLexiconProvider::PackDirectory(L"social-expression") / L"entries.tsv",
        tekito::CandidateSourceSocialExpression);
    Require(social.IsLoaded(), "Social Expressions Data Pack loads through its index");
    bool socialFound = false;
    social.Find({tekito::LexiconQuery::Kind::Exact, L"lol"}, [&](const auto& entry) {
        socialFound = socialFound ||
            (HasFlag(entry.sourceFlags, tekito::CandidateSourceSocialExpression) &&
             entry.socialRange >= tekito::SocialRangeFamiliar &&
             (entry.candidateLabel == tekito::SemanticLabel::Reaction ||
              entry.candidateLabel == tekito::SemanticLabel::Emoji));
    });
    Require(socialFound, "Social Expressions provider serves chat candidates without auto policy");

    const tekito::ExternalSlangProvider phonetic(
        tekito::ExternalLexiconProvider::PackDirectory(L"japanese-phonetic") / L"entries.tsv",
        tekito::CandidateSourceJapanesePhonetic);
    Require(phonetic.IsLoaded(), "Japanese Phonetic Data Pack loads through its index");
    bool phoneticFound = false;
    phonetic.Find({tekito::LexiconQuery::Kind::Exact, L"konpyuutaa"}, [&](const auto& entry) {
        phoneticFound = entry.candidate == L"computer" &&
                        HasFlag(entry.candidatePolicyFlags, tekito::CandidatePolicySuggestOnly);
    });
    Require(phoneticFound, "Japanese phonetic provider serves suggestion-only candidates");
}

void TestChatAbbreviationsStayAsTyped() {
    // Spellings in TEKITO's own slang list are meant, even when they are one
    // edit away from a common word (brb/bob, omw/bmw, thx/the).
    const auto engine = tekito::CreateDefaultConversionEngine();
    for (const int range : {0, 1}) {
        for (const auto* raw : {L"brb", L"cya", L"omw", L"thx", L"ngl", L"tbh", L"smh",
                                L"pls", L"ttyl", L"yall", L"gonna", L"idk"}) {
            tekito::ConversionRequest request;
            request.rawText = raw;
            request.options.socialExpressionRange = range;
            const auto candidates = engine->Convert(request).candidates;
            tekito::InputStateMachine state;
            state.BeginOrUpdate(raw, candidates);
            if (state.OnSpace().text != std::wstring(raw) + L" ") {
                std::wcerr << L"[chat abbreviation] " << raw << L" range " << range << L"\n";
            }
            Require(!candidates.empty() && candidates.front().text == raw &&
                        candidates.front().isProtected,
                    "a listed chat abbreviation is kept as typed");
        }
    }

    const auto brb = engine->Convert({L"brb"}).candidates;
    const auto meaning = std::find_if(brb.begin(), brb.end(), [](const auto& candidate) {
        return candidate.text == L"be right back";
    });
    const auto lookAlike = std::find_if(brb.begin(), brb.end(), [](const auto& candidate) {
        return candidate.text == L"bob";
    });
    Require(meaning != brb.end() && (lookAlike == brb.end() || meaning < lookAlike),
            "an abbreviation's meaning is offered ahead of spelling look-alikes");

    // Wiktionary lists "teh" too, but only TEKITO's own lists protect a word.
    tekito::InputStateMachine typo;
    typo.BeginOrUpdate(L"teh", engine->Convert({L"teh"}).candidates);
    Require(typo.OnSpace().text == L"the ",
            "a common typo that an external slang list contains is still corrected");
}

void TestTypoModelBoundary() {
    const tekito::TypoModel model;
    const auto repeated = model.Compare(L"meeet", L"meet");
    Require(repeated.repeatedKey && repeated.distance == 1,
            "typo model detects repeated-key evidence generically");

    const auto transposition = model.Compare(L"hte", L"the");
    Require(transposition.transposition && transposition.distance == 1,
            "typo model detects transposition evidence generically");

    const auto neighbor = model.Compare(L"gopd", L"good");
    Require(neighbor.qwertyNeighbor && neighbor.distance == 1,
            "typo model detects QWERTY-neighbor evidence generically");
}

void TestTargetTextCandidateDiagnostics() {
    struct Case {
        std::wstring_view raw;
        std::wstring_view target;
    };
    constexpr Case cases[] = {
        {L"wrold", L"world"},
        {L"tommorrow", L"tomorrow"},
        {L"yestaday", L"yesterday"},
        {L"definately", L"definitely"},
        {L"recieved", L"received"},
        {L"mesage", L"message"},
        {L"seperate", L"separate"},
        {L"adress", L"address"},
    };

    const tekito::ExternalLexiconProvider external(
        tekito::ExternalLexiconProvider::DefaultPath());
    const tekito::BuiltinLexiconProvider builtin;
    const tekito::CompositeLexiconProvider lexicon(external, builtin);
    const tekito::CandidateEngine engine;
    const tekito::TypoModel typoModel;
    const tekito::SpaceBoundaryPolicy spacePolicy;

    std::cout << "[diagnostic] Candidate generation / lexicon / typo / SpaceBoundary\n";
    for (const auto& testCase : cases) {
        bool targetInLexicon = false;
        lexicon.Find({tekito::LexiconQuery::Kind::Exact, testCase.target},
                     [&](const auto&) { targetInLexicon = true; });

        const auto evidence = typoModel.Compare(testCase.raw, testCase.target);
        const auto candidates = engine.Generate(testCase.raw);
        const auto targetIt = std::find_if(
            candidates.begin(), candidates.end(), [&](const auto& candidate) {
                return candidate.text == testCase.target;
            });
        const auto accepted = spacePolicy.SelectCorrection(testCase.raw, candidates);

        std::cout << "  " << NarrowAscii(testCase.raw) << " -> "
                  << NarrowAscii(testCase.target) << "\n"
                  << "    lexicon_exact=" << (targetInLexicon ? "yes" : "no")
                  << " typo_distance=" << evidence.distance
                  << " repeated=" << (evidence.repeatedKey ? "yes" : "no")
                  << " transposition=" << (evidence.transposition ? "yes" : "no")
                  << " missing=" << (evidence.missingCharacter ? "yes" : "no")
                  << " extra=" << (evidence.extraCharacter ? "yes" : "no")
                  << " qwerty=" << (evidence.qwertyNeighbor ? "yes" : "no") << "\n";
        std::cout << "    candidates=" << candidates.size() << "\n";
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            std::cout << "      [" << index << "] " << NarrowAscii(candidate.text)
                      << " original=" << (candidate.isOriginal ? "yes" : "no")
                      << " policy=" << candidate.policyFlags
                      << " source=" << candidate.sourceFlags << "\n";
        }

        Require(!candidates.empty(), "diagnostic case produces a candidate list");
        if (targetIt == candidates.end()) {
            std::cout << "    diagnosis=candidate_not_generated\n";
            Require(false, "target text correction candidate is generated");
        } else if (targetIt != candidates.begin()) {
            std::cout << "    diagnosis=candidate_generated_lower_rank\n";
            Require(false, "target text correction ranks first");
        } else if (!accepted.has_value()) {
            std::cout << "    diagnosis=space_boundary_rejected\n";
            Require(false, "target text correction is accepted at Space boundary");
        } else {
            std::cout << "    diagnosis=space_boundary_accepts_target\n";
        }
    }

    constexpr std::wstring_view protectedWords[] = {
        L"meet", L"met", L"tomorrow", L"yesterday", L"Nice", L"Neme", L"OpenAI"};
    for (const auto raw : protectedWords) {
        const auto candidates = engine.Generate(raw);
        Require(!candidates.empty() && candidates.front().text == raw,
                "target text keeps a correct or protected word as the first candidate");
        std::cout << "  preserve " << NarrowAscii(raw) << " -> "
                  << NarrowAscii(candidates.front().text) << "\n";
    }
}

// How English is written, whatever the spelling search finds: "I",
// contractions without the apostrophe, two words run together, names
// written with capitals, a second capital by mistake.
void TestEnglishWritingRules() {
    const auto engine = tekito::CreateDefaultConversionEngine();
    const tekito::SpaceBoundaryPolicy space;
    const auto spaceGives = [&](std::wstring_view raw, std::wstring_view context = L"so ") {
        tekito::ConversionRequest request;
        request.rawText = std::wstring(raw);
        request.context.precedingText = std::wstring(context);
        const auto candidates = engine->Convert(request).candidates;
        const auto selected = space.SelectCorrection(raw, candidates);
        return selected ? candidates[*selected].text : std::wstring(raw);
    };
    const auto offers = [&](std::wstring_view raw, std::wstring_view text) {
        tekito::ConversionRequest request;
        request.rawText = std::wstring(raw);
        request.context.precedingText = L"so ";
        const auto candidates = engine->Convert(request).candidates;
        return candidates.size() > 1 && candidates[1].text == text;
    };
    Require(spaceGives(L"i") == L"I", "i is written I");
    Require(spaceGives(L"dont") == L"don't", "dont gets its apostrophe");
    Require(spaceGives(L"youre") == L"you're", "youre is you're, not your");
    Require(spaceGives(L"theyre") == L"they're", "theyre is they're, not there");
    Require(spaceGives(L"thats") == L"that's", "thats is that's");
    Require(spaceGives(L"im") == L"I'm", "im is I'm");
    Require(spaceGives(L"cant") == L"cant" && offers(L"cant", L"can't"),
            "cant is a word too: kept, with can't offered first");
    Require(spaceGives(L"heros") != L"hero's", "a plural typo is not made a possessive");
    Require(spaceGives(L"ofthe") == L"of the", "ofthe keeps both words");
    Require(spaceGives(L"alot") == L"a lot", "alot is a lot");
    Require(spaceGives(L"powerfull") != L"power full", "a typo is not split into two words");
    Require(spaceGives(L"monday") == L"Monday", "days are written with a capital");
    Require(spaceGives(L"japanese") == L"Japanese", "languages are written with a capital");
    Require(spaceGives(L"iphone") == L"iPhone", "a product name keeps its capitals");
    Require(spaceGives(L"march") == L"march" && offers(L"march", L"March"),
            "march is a word too: kept, with March offered");
    Require(spaceGives(L"THe") == L"The", "a second capital by mistake is taken back");
    Require(spaceGives(L"NASA") == L"NASA", "an acronym stays as typed");
}

// A word typed twice, the next words, and Space twice for a period.
void TestEnglishWritingOptions() {
    const auto engine = tekito::CreateDefaultConversionEngine();
    const tekito::SpaceBoundaryPolicy space;
    const auto convert = [&](std::wstring_view raw, std::wstring_view context, bool options = true) {
        tekito::ConversionRequest request;
        request.rawText = std::wstring(raw);
        request.context.precedingText = std::wstring(context);
        request.options.doubledWords = options;
        request.options.nextWordPrediction = options;
        return engine->Convert(request).candidates;
    };
    const auto spaceGives = [&](std::wstring_view raw, std::wstring_view context, bool options = true) {
        const auto candidates = convert(raw, context, options);
        const auto selected = space.SelectCorrection(raw, candidates);
        return selected ? candidates[*selected].text : std::wstring(raw);
    };
    Require(spaceGives(L"the", L"I saw the ").empty(), "the the: the second one is taken out");
    Require(spaceGives(L"the", L"I saw the ", false) == L"the", "doubled words can be left alone");
    Require(spaceGives(L"that", L"I said that ") == L"that", "that that may be meant: kept");
    const auto thatThat = convert(L"that", L"I said that ");
    Require(std::any_of(thatThat.begin(), thatThat.end(), [](const tekito::Candidate& candidate) {
                return candidate.text.empty() && (candidate.sourceFlags & tekito::CandidateSourceDoubledWord) != 0;
            }),
            "that that: taking one out is offered");
    Require(spaceGives(L"the", L"the end. Then the ") .empty(), "doubled across a sentence too");
    Require(spaceGives(L"the", L"the. ") == L"the", "not doubled across punctuation");

    const auto next = convert(L"t", L"I want ");
    const auto to = std::find_if(next.begin(), next.end(), [](const tekito::Candidate& candidate) {
        return candidate.text == L"to" && (candidate.sourceFlags & tekito::CandidateSourcePrediction) != 0;
    });
    Require(to != next.end() && to - next.begin() <= 3, "I want t: to is predicted on the first page");
    Require(!next.empty() && (next.front().sourceFlags & tekito::CandidateSourcePrediction) == 0,
            "a prediction is never put first");
    const auto off = convert(L"t", L"I want ", false);
    Require(std::none_of(off.begin(), off.end(), [](const tekito::Candidate& candidate) {
                return (candidate.sourceFlags & tekito::CandidateSourcePrediction) != 0;
            }),
            "predictions can be turned off");

    // The empty candidate leaves the space already typed.
    std::vector<tekito::Candidate> removal{
        {L"", tekito::SemanticLabel::None, false, 100.0, false, tekito::CandidatePolicyNormalize,
         tekito::CandidateSourceDoubledWord},
        {L"the", tekito::SemanticLabel::Original, true},
    };
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"the", removal);
    auto action = state.OnSpace();
    Require(action.kind == tekito::ActionKind::ReplaceComposition && action.text.empty(),
            "Space takes the doubled word out, adding no space");
    action = state.OnBackspace();
    Require(action.kind == tekito::ActionKind::RestoreOriginal && action.text == L"the",
            "Backspace brings the doubled word back");
    const tekito::AutoApplyPolicy autoApply;
    Require(!autoApply.SelectForBoundary(L"the", removal, std::nullopt),
            "punctuation does not take a doubled word out");

    std::vector<tekito::Candidate> kept{
        {L"hello", tekito::SemanticLabel::Original, true},
        {L"hallo", tekito::SemanticLabel::None, false, 90.0, false, tekito::CandidatePolicySuggestOnly},
    };
    state.SetDoubleSpacePeriod(true);
    state.BeginOrUpdate(L"hello", kept);
    action = state.OnSpace();
    Require(action.text == L"hello ", "first Space ends the word");
    action = state.OnSpace();
    Require(action.kind == tekito::ActionKind::CommitAndStartNext && action.text == L"hello. ",
            "second Space puts a period");
    state.BeginOrUpdate(L"hello", kept);
    (void)state.OnSpace();
    (void)state.OnTab();
    action = state.OnSpace();
    Require(action.text != L"hello. ", "Space after Tab does not put a period");
    state.SetDoubleSpacePeriod(false);
    state.BeginOrUpdate(L"hello", kept);
    (void)state.OnSpace();
    action = state.OnSpace();
    Require(action.kind == tekito::ActionKind::ReplaceComposition, "with it off, Space twice goes to the next choice");
}

class TestDictionaryProvider final : public tekito::dictionary::IDictionaryProvider {
public:
    [[nodiscard]] std::optional<tekito::dictionary::DictionaryEntry> Find(
        std::wstring_view entryId) const override {
        if (entryId != L"external:hello") return std::nullopt;
        return tekito::dictionary::DictionaryEntry{
            L"external:hello", L"hello", L"/huh-loh/", L"interjection",
            L"a greeting", L"Hello, world."};
    }
};

class TestPronunciationProvider final : public tekito::dictionary::IPronunciationProvider {
public:
    [[nodiscard]] std::wstring Find(std::wstring_view headword) const override {
        return headword == L"hello" ? L"HH AH0 L OW1" : L"";
    }
};

void TestDictionaryServiceBoundary() {
    TestDictionaryProvider provider;
    const tekito::dictionary::DictionaryService service(provider);
    const tekito::Candidate candidate{
        L"hello", tekito::SemanticLabel::None, false, 1.0, false,
        tekito::CandidatePolicyNone, tekito::CandidateSourceExternalLexicon,
        L"external:hello"};
    const auto entry = service.Lookup(candidate);
    Require(entry.has_value() && entry->headword == L"hello" &&
                entry->partOfSpeech == L"interjection",
            "dictionary service resolves candidate metadata through a separate provider");

    const tekito::Candidate withoutDictionaryId{L"hello"};
    Require(!service.Lookup(withoutDictionaryId).has_value(),
            "dictionary service does not perform a raw text lookup without an entry ID");

    TestPronunciationProvider pronunciation;
    const tekito::dictionary::DictionaryService enriched(provider, pronunciation);
    const auto pronounced = enriched.Lookup(candidate);
    Require(pronounced && pronounced->pronunciation == L"/huh-loh/",
            "dictionary pronunciation already present is not overwritten");
}

void TestLocalDataPackProviders() {
    const tekito::dictionary::ExternalDictionaryProvider dictionaryProvider(
        tekito::dictionary::ExternalDictionaryProvider::DefaultPath());
    Require(dictionaryProvider.IsLoaded(),
            "dictionary display pack is loaded from the configured data root");
    const auto world = dictionaryProvider.Find(L"external:world");
    const auto address = dictionaryProvider.Find(L"external:address");
    Require(world && world->headword == L"world" && !world->definition.empty() &&
                address && address->partOfSpeech == L"noun",
            "dictionary provider resolves display data by candidate entry ID");

    const tekito::ExternalFrequencyProvider frequency(
        tekito::ExternalFrequencyProvider::DefaultPath());
    Require(frequency.IsLoaded() && frequency.Score(L"wald") > 0.0 &&
                frequency.Score(L"world") > frequency.Score(L"wald"),
            "frequency provider reads local indexed scores without treating them as validity");

    const tekito::ExternalLexiconProvider lexicon(tekito::ExternalLexiconProvider::DefaultPath());
    bool nearWorld = false;
    lexicon.Find({tekito::LexiconQuery::Kind::Candidate, L"wald", 512}, [&](const auto& entry) {
        nearWorld = nearWorld || entry.raw == L"world";
    });
    Require(nearWorld, "external lexicon candidate query finds a bounded near word");
    const tekito::ExternalPhraseContextProvider phrase(
        tekito::ExternalPhraseContextProvider::DefaultPath());
    if (OptionalPackInstalled(L"phrase")) {
        Require(phrase.IsLoaded() && phrase.Score(L"again", L"met him") > 0.0,
                "phrase provider reads an exact local context score");
        Require(phrase.Score(L"to", L"I want") > phrase.Score(L"too", L"I want") &&
                    phrase.Score(L"to", L"Me") > 0.0 && phrase.Score(L"too", L"Me") > 0.0,
                "phrase provider exposes real valid-word context alternatives to the ranker");
    }
    const tekito::ExternalSlangProvider slang(tekito::ExternalSlangProvider::DefaultPath());
    bool foundSlang = false;
    slang.Find({tekito::LexiconQuery::Kind::Exact, L"gonna"}, [&](const auto& entry) {
        foundSlang = entry.sourceFlags == tekito::CandidateSourceTekitoOwnedSlang &&
                     entry.candidate == L"going to";
    });
    Require(slang.IsLoaded(), "TEKITO-owned slang pack is loaded from the data root");
    Require(foundSlang, "TEKITO-owned slang provider returns the expected expansion metadata");

    const auto dataRoot = tekito::ExternalLexiconProvider::DataPackRoot();
    const tekito::ExternalSlangProvider wiktionary(
        tekito::FindDataPack(dataRoot, L"wiktionary-slang") / L"entries.tsv",
        tekito::CandidateSourceWiktionary);
    bool foundAbbreviation = false;
    wiktionary.Find({tekito::LexiconQuery::Kind::Exact, L"brb"}, [&](const auto& entry) {
        foundAbbreviation = foundAbbreviation ||
                            entry.sourceFlags == tekito::CandidateSourceWiktionary;
    });
    Require(wiktionary.IsLoaded() && foundAbbreviation,
            "Wiktionary slang pack is queried through its independent local provider");

    {
        const tekito::ExternalSlangProvider proper(
            tekito::FindDataPack(dataRoot, L"proper-nouns") / L"entries.tsv",
            tekito::CandidateSourceProperNoun);
        bool foundTokyo = false;
        proper.Find({tekito::LexiconQuery::Kind::Exact, L"tokyo"}, [&](const auto& entry) {
            foundTokyo = entry.candidate == L"Tokyo" &&
                         HasFlag(entry.candidatePolicyFlags, tekito::CandidatePolicySuggestOnly);
        });
        Require(proper.IsLoaded() && foundTokyo,
                "GeoNames proper noun pack preserves display spelling without forced replacement");
    }

    const tekito::ExternalSlangProvider emoji(
        tekito::FindDataPack(dataRoot, L"emoji") / L"entries.tsv", tekito::CandidateSourceEmoji);
    bool foundEmoji = false;
    emoji.Find({tekito::LexiconQuery::Kind::Exact, L"grinning"}, [&](const auto& entry) {
        foundEmoji = foundEmoji ||
                     HasFlag(entry.candidatePolicyFlags, tekito::CandidatePolicyEmoji) &&
                     HasFlag(entry.candidatePolicyFlags, tekito::CandidatePolicySuggestOnly);
    });
    Require(emoji.IsLoaded() && foundEmoji,
            "CLDR emoji pack returns suggest-only emoji candidates through its provider");
    bool foundNonEmojiAnnotation = false;
    emoji.Find({tekito::LexiconQuery::Kind::Exact, L"accent"}, [&](const auto&) {
        foundNonEmojiAnnotation = true;
    });
    Require(!foundNonEmojiAnnotation,
            "CLDR symbol annotations are not mislabeled as emoji candidates");

    const tekito::dictionary::ExternalPronunciationProvider pronunciation(
        tekito::dictionary::ExternalPronunciationProvider::DefaultPath());
    Require(pronunciation.IsLoaded() && pronunciation.Find(L"hello").find(L"HH") != std::wstring::npos,
            "CMUdict pronunciation pack is resolved by indexed headword lookup");
    const tekito::BuiltinLexiconProvider builtin;
    const tekito::CompositeLexiconProvider composite(lexicon, builtin);
    const tekito::CandidateGenerator weakGenerator(composite, frequency,
                                                    phrase);
    const auto weakCandidates = weakGenerator.Generate(L"wrold");
    Require(std::any_of(weakCandidates.begin(), weakCandidates.end(), [](const auto& candidate) {
                return candidate.text == L"world";
            }),
            "candidate generator consumes the bounded near-word query");

    const auto engine = tekito::CreateDefaultConversionEngine();
    const auto result = engine->Convert({L"yestaday", {false, L"I met him"}});
    Require(std::any_of(result.candidates.begin(), result.candidates.end(), [](const auto& candidate) {
                return candidate.text == L"yesterday";
            }),
            "default engine keeps local phrase provider connected to candidate generation");

    tekito::ConversionRequest socialRequest;
    socialRequest.rawText = L"lol";
    socialRequest.options.socialExpressionRange = 4;
    const auto socialCandidates = engine->Convert(socialRequest).candidates;
    const auto hasSocialText = std::any_of(
        socialCandidates.begin(), socialCandidates.end(), [](const auto& candidate) {
            return candidate.text == L"XD" || candidate.text == L"( ´艸｀)";
        });
    const auto hasSocialEmoji = std::any_of(
        socialCandidates.begin(), socialCandidates.end(), [](const auto& candidate) {
            return candidate.label == tekito::SemanticLabel::Emoji &&
                   (candidate.text == L"\U0001F602" || candidate.text == L"\U0001F923");
        });
    Require(hasSocialText && hasSocialEmoji,
            "expanded social range exposes LOL emoticon, kaomoji, and emoji candidates");

    tekito::ConversionRequest arrowRequest;
    arrowRequest.rawText = L"->";
    arrowRequest.options.socialExpressionRange = 3;
    const auto arrowCandidates = engine->Convert(arrowRequest).candidates;
    const auto arrow = std::find_if(arrowCandidates.begin(), arrowCandidates.end(),
                                    [](const auto& candidate) {
                                        return candidate.text == L"\u2192";
                                    });
    Require(arrow != arrowCandidates.end() && !arrow->isOriginal &&
                HasFlag(arrow->policyFlags, tekito::CandidatePolicySuggestOnly) &&
                HasFlag(arrow->sourceFlags, tekito::CandidateSourceSocialExpression),
            "ASCII arrow shorthand exposes a reversible Unicode arrow candidate");

    tekito::ConversionRequest kaomojiRequest;
    kaomojiRequest.rawText = L":shrug";
    kaomojiRequest.options.socialExpressionRange = 2;
    const auto kaomojiCandidates = engine->Convert(kaomojiRequest).candidates;
    const auto shrug = std::find_if(kaomojiCandidates.begin(), kaomojiCandidates.end(),
                                    [](const auto& candidate) {
                                        return candidate.text == L"\u00AF\\_(\u30C4)_/\u00AF";
                                    });
    Require(shrug != kaomojiCandidates.end() &&
                HasFlag(shrug->policyFlags, tekito::CandidatePolicySuggestOnly) &&
                shrug->socialRange == tekito::SocialRangeFamiliar,
            "explicit social syntax exposes a bounded suggestion-only kaomoji");

    const auto firstEmoji = std::find_if(socialCandidates.begin(), socialCandidates.end(),
                                         [](const auto& candidate) {
                                             return candidate.label == tekito::SemanticLabel::Emoji;
                                         });
    const auto firstSocialText = std::find_if(socialCandidates.begin(), socialCandidates.end(),
                                              [](const auto& candidate) {
                                                  return !candidate.isOriginal &&
                                                         candidate.socialRange !=
                                                             tekito::SocialRangeUnspecified &&
                                                         candidate.label !=
                                                             tekito::SemanticLabel::Emoji;
                                              });
    Require(firstEmoji != socialCandidates.end() &&
                (firstSocialText == socialCandidates.end() || firstEmoji < firstSocialText),
            "casual social input prioritizes emoji candidates");

    tekito::ConversionRequest happyRequest;
    happyRequest.rawText = L"happy";
    happyRequest.options.socialExpressionRange = 1;
    const auto happyCandidates = engine->Convert(happyRequest).candidates;
    const auto happyEmoji = std::find_if(happyCandidates.begin(), happyCandidates.end(),
                                         [](const auto& candidate) {
                                             return candidate.label == tekito::SemanticLabel::Emoji;
                                         });
    Require(happyEmoji != happyCandidates.end() &&
                static_cast<std::size_t>(happyEmoji - happyCandidates.begin()) < 5,
            "normal words keep a relevant emoji on the first candidate page");

    const auto validExact = engine->Convert({L"wald"});
    Require(!validExact.candidates.empty() && validExact.candidates.front().text == L"wald",
            "a valid SCOWL word remains raw without a strong context advantage");
    const auto transposedWorld = engine->Convert({L"wrold"});
    Require(!transposedWorld.candidates.empty() &&
                transposedWorld.candidates.front().text == L"world",
            "general transposition evidence corrects a non-word without a word exception");

    const auto hallo = engine->Convert({L"hallo"});
    Require(!hallo.candidates.empty() && hallo.candidates.front().text == L"hello",
            "default engine ranks frequent hello above rare halo");
    tekito::InputStateMachine contextualBoundary;
    if (OptionalPackInstalled(L"phrase")) {
        // The words before alone do not replace a word typed right: they
        // rewrote real text ("buy them," to "buy the") far more often than
        // they caught a slip. The word that fits is offered instead.
        const auto tooInWant = engine->Convert({L"too", {false, L"I want", L""}});
        Require(!tooInWant.candidates.empty() && tooInWant.candidates.front().text == L"too" &&
                    std::any_of(tooInWant.candidates.begin(), tooInWant.candidates.end(),
                                [](const auto& candidate) { return candidate.text == L"to"; }),
                "after I want, too stays first and to is offered");
        contextualBoundary.BeginOrUpdate(L"too", tooInWant.candidates);
        Require(contextualBoundary.OnSpace().text == L"too ",
                "Space keeps a valid word even when context favors one a letter away");
        const auto themAfterBuy = engine->Convert({L"them", {false, L"But people want to buy", L""}});
        contextualBoundary.BeginOrUpdate(L"them", themAfterBuy.candidates);
        Require(contextualBoundary.OnSpace().text == L"them ", "Space keeps them after buy");
    }
    const auto tooAfterMe = engine->Convert({L"too", {false, L"Me", L""}});
    Require(!tooAfterMe.candidates.empty() && tooAfterMe.candidates.front().text == L"too" &&
                std::none_of(tooAfterMe.candidates.begin(), tooAfterMe.candidates.end(),
                             [](const auto& candidate) {
                                 return HasFlag(candidate.policyFlags,
                                                tekito::CandidatePolicyCorrect);
                             }),
            "raw too remains primary after Me without an accepted correction");
    contextualBoundary.BeginOrUpdate(L"too", tooAfterMe.candidates);
    Require(contextualBoundary.OnSpace().text == L"too ",
            "Space keeps a valid raw word when context does not favor a correction");
    const auto tooBeforeExpensive =
        engine->Convert({L"too", {false, L"This is", L"expensive"}});
    Require(!tooBeforeExpensive.candidates.empty() &&
                tooBeforeExpensive.candidates.front().text == L"too",
            "raw too remains primary in This is too expensive context");
    contextualBoundary.BeginOrUpdate(L"too", tooBeforeExpensive.candidates);
    Require(contextualBoundary.OnSpace().text == L"too ",
            "following context keeps too at the Space boundary");
    const auto tooWithoutContext = engine->Convert({L"too"});
    Require(!tooWithoutContext.candidates.empty() &&
                tooWithoutContext.candidates.front().text == L"too",
            "Frequency alone does not promote a valid-word correction");

    const auto h = engine->Convert({L"h"});
    Require(h.candidates.size() > 1 && h.candidates.front().text == L"h",
            "one typed character keeps raw while showing suggestions");

    const auto ha = engine->Convert({L"ha"});
    Require(ha.candidates.size() > 1 && ha.candidates.front().text == L"ha",
            "a valid short prefix keeps raw first while still offering candidates");
    const bool hasCompletion = std::any_of(
        ha.candidates.begin(), ha.candidates.end(), [](const auto& candidate) {
        return HasFlag(candidate.sourceFlags, tekito::CandidateSourceCompletion);
    });
    Require(hasCompletion,
            "a short prefix offers completion candidates without fixing a product ranking");
    contextualBoundary.BeginOrUpdate(L"ha", ha.candidates);
    Require(contextualBoundary.OnSpace().text == L"ha ",
            "short valid-word suggestions do not replace raw at the Space boundary");

    const auto hall = engine->Convert({L"hall"});
    Require(hall.candidates.size() > 1 && hall.candidates.front().text == L"hall",
            "a valid longer prefix keeps raw first while still offering candidates");
    const bool hasHello = std::any_of(hall.candidates.begin(), hall.candidates.end(), [](const auto& candidate) {
        return candidate.text == L"hello";
    });
    if (!hasHello) {
        std::wcerr << L"[hall candidates]";
        for (const auto& candidate : hall.candidates) std::wcerr << L" " << candidate.text;
        std::wcerr << L"\n";
    }
    Require(hasHello,
            "fuzzy candidate generation offers hello for hall without auto-applying it");
    contextualBoundary.BeginOrUpdate(L"hall", hall.candidates);
    Require(contextualBoundary.OnSpace().text == L"hall ",
            "fuzzy suggestions for a valid word remain opt-in");

    struct ContextCase {
        const wchar_t* raw;
        const wchar_t* previous;
        const wchar_t* following;
    };
    for (const auto& item : {
             ContextCase{L"met", L"I", L"him yesterday"},
             ContextCase{L"meet", L"I will", L"him tomorrow"},
             ContextCase{L"form", L"Fill out the", L""},
             ContextCase{L"from", L"I came", L"Tokyo"},
         }) {
        const auto preserved = engine->Convert(
            {item.raw, {false, item.previous, item.following}});
        if (preserved.candidates.empty() || preserved.candidates.front().text != item.raw) {
            std::wcerr << L"[context diagnostic] " << item.previous << L" [" << item.raw
                       << L"] " << item.following << L":";
            for (const auto& candidate : preserved.candidates) {
                std::wcerr << L" " << candidate.text << L"(" << candidate.policyFlags << L")";
            }
            std::wcerr << L"\n";
        }
        Require(!preserved.candidates.empty() &&
                    preserved.candidates.front().text == item.raw,
                "valid contextual words remain unchanged without candidate advantage");
    }

    tekito::ConversionRequest disabledRequest;
    disabledRequest.rawText = L"neme";
    disabledRequest.options.correctionEnabled = false;
    disabledRequest.options.completionEnabled = false;
    const auto disabled = engine->Convert(disabledRequest);
    Require(!disabled.candidates.empty() && disabled.candidates.front().text == L"neme" &&
                std::none_of(disabled.candidates.begin(), disabled.candidates.end(),
                             [](const auto& candidate) {
                                 return HasFlag(candidate.policyFlags,
                                                tekito::CandidatePolicyCorrect);
                             }),
            "Input settings can disable correction without changing the key contract");
}

void TestUserDictionaryImportExport() {
    const auto path = std::filesystem::temp_directory_path() / L"tekito-user-dictionary-v1.tsv";
    std::error_code error;
    std::filesystem::remove(path, error);

    tekito::UserDictionary source;
    Require(source.Add({7, L"na\tme", L"N\u0061me", tekito::CandidatePolicyProtect,
                        true, false}),
            "user dictionary export accepts a structured entry");
    Require(tekito::userdata::ExportUserDictionary(source, path),
            "user dictionary export writes the explicit versioned format");

    tekito::UserDictionary loaded;
    Require(tekito::userdata::ImportUserDictionary(path, loaded),
            "user dictionary import reads the explicit versioned format");
    const auto* entry = loaded.Find(7);
    Require(entry && entry->raw == L"na\tme" && entry->candidate == L"Name" &&
                HasFlag(entry->policyFlags, tekito::CandidatePolicyProtect) &&
                entry->caseSensitive && !entry->enabled,
            "user dictionary import preserves escaped text and policy metadata");
    std::filesystem::remove(path, error);
}

void TestUserLearningStore() {
    tekito::UserLearningStore learning;
    learning.RecordCandidateSelection(L"tomorrow");
    learning.RecordCandidateSelection(L"tomorrow");
    learning.RecordUndo(L"tomorrow");
    learning.RecordRawKeep(L"meet");
    Require(learning.Score(L"tomorrow") > learning.Score(L"meet") &&
                learning.Score(L"unknown") == 0.0,
            "user learning exposes candidate preference without storing input text");
    Require(learning.Entries().size() == 2 && learning.Entries()[0].word == L"meet" &&
                learning.Entries()[1].word == L"tomorrow",
            "user learning stays sorted for allocation-free binary score lookup");

    tekito::UserLearningProvider provider(learning);
    Require(provider.Score(L"tomorrow") == learning.Score(L"tomorrow"),
            "user learning provider reads the in-memory store");
    provider.SetEnabled(false);
    Require(provider.Score(L"tomorrow") == 0.0,
            "disabled user learning provider contributes no ranking signal");
    provider.SetEnabled(true);
    Require(provider.Score(L"tomorrow") > 0.0,
            "user learning provider can be re-enabled without losing stored data");

    for (int count = 0; count < 6; ++count) {
        learning.RecordExposure(L"hte", L"the");
        learning.RecordSelection(L"hte", L"the");
    }
    learning.RecordExposure(L"hte", L"thy");
    learning.RecordNonSelection(L"hte", L"thy");
    Require(provider.Score(L"hte", L"the") > provider.Score(L"hte", L"thy") &&
                provider.Score(L"hte", L"the") > 0.20,
            "pairwise online learning promotes a repeatedly selected correction");
    const auto scoreBeforeUndo = provider.Score(L"hte", L"the");
    learning.RecordUndo(L"hte", L"the");
    Require(provider.Score(L"hte", L"the") < scoreBeforeUndo,
            "pairwise online learning reduces a correction after undo");
    Require(!provider.Undone(L"hte", L"the"),
            "one undo of a correction taken many times does not stop it");
    learning.RecordUndo(L"teh", L"the");
    Require(provider.Undone(L"teh", L"the") && provider.Undone(L"TEH", L"The"),
            "one undo of a correction never taken stops it, whatever the case");
    provider.SetEnabled(false);
    Require(!provider.Undone(L"teh", L"the"), "disabled user learning stops nothing");
}

void TestSqliteUserLearningRepository() {
    const auto path = std::filesystem::temp_directory_path() / L"tekito-user-learning-test.db";
    std::error_code error;
    std::filesystem::remove(path, error);

    tekito::UserLearningStore source;
    source.RecordCandidateSelection(L"tomorrow");
    source.RecordUndo(L"tomorrow");
    source.RecordExposure(L"hte", L"the");
    source.RecordSelection(L"hte", L"the");
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open() && repository.SaveLearning(source),
                "sqlite repository saves user learning aggregates");
    }
    tekito::UserLearningStore loaded;
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open() && repository.LoadLearning(loaded),
                "sqlite repository loads user learning aggregates");
    }
    Require(loaded.Entries().size() == 1 && loaded.Score(L"tomorrow") == 0.5 &&
                loaded.Preferences().size() == 1 && loaded.Score(L"hte", L"the") == 0.0,
            "sqlite repository restores learning counts");
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open() && repository.ResetLearning(),
                "sqlite repository resets learning independently");
        tekito::UserLearningStore reset;
        Require(repository.LoadLearning(reset) && reset.Entries().empty() &&
                    reset.Preferences().empty(),
            "learning reset is immediately observable as an empty snapshot");
    }
    std::filesystem::remove(path, error);
}

// Postal codes added in Settings: Japan Post's CSV becomes a sorted pack.
void TestImeDictionaryImport() {
    using tekito::japanese::UserWordAction;
    using tekito::japanese::UserWordKind;
    // Microsoft IME's export: UTF-16 with a byte order mark and "!" headers.
    const std::wstring microsoft =
        L"!Microsoft IME Dictionary Tool\r\n!Version:\r\n!Format:WORDLIST\r\n\r\n"
        L"てきと\tTEKITO\t固有名詞\r\n"
        L"やまだ\t山田\t姓\r\n"
        L"たろう\t太郎\t名\r\n"
        L"ヘンカン\t返還\t抑制単語\r\n"
        L"abc\tABC\t名詞\r\n";
    std::string bytes = "\xFF\xFE";
    for (const wchar_t c : microsoft) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>(c >> 8));
    }
    const auto ms = tekito::userdata::ParseImeDictionary(bytes);
    Require(ms && ms->words.size() == 4 && ms->skipped == 1, "Microsoft IME's export is read, its headers skipped");
    Require(ms->words[0].reading == L"てきと" && ms->words[0].surface == L"TEKITO" &&
                ms->words[0].kind == UserWordKind::ProperNoun,
            "a proper noun");
    Require(ms->words[1].kind == UserWordKind::Surname && ms->words[2].kind == UserWordKind::GivenName,
            "surnames and given names");
    Require(ms->words[3].reading == L"へんかん" && ms->words[3].action == UserWordAction::Suppress,
            "a katakana reading becomes hiragana; a suppressed word stays suppressed");

    // Google Japanese Input's export: UTF-8, with a comment column.
    const std::string google = "とうきょうえき\t東京駅\t地名\tメモ\nかおもじ\t(^^)\t顔文字\n";
    const auto g = tekito::userdata::ParseImeDictionary(google);
    Require(g && g->words.size() == 2 && g->words[0].kind == UserWordKind::Place &&
                g->words[1].kind == UserWordKind::Symbol,
            "Google Japanese Input's export is read");

    // ATOK's parts of speech end in "*".
    const auto atok = tekito::userdata::ParseImeDictionary("!!ATOK_TANGO_TEXT_HEADER_1\nさとう\t佐藤\t固有人名*\n");
    Require(atok && atok->words.size() == 1 && atok->words[0].kind == UserWordKind::Person, "ATOK's export is read");

    Require(!tekito::userdata::ParseImeDictionary("not a dictionary"), "other files give nothing");

    std::vector<tekito::japanese::UserWord> words{{L"てきと", L"TEKITO", UserWordKind::Noun, UserWordAction::First}};
    Require(tekito::userdata::MergeImeWords(words, ms->words) == 3 && words.size() == 4,
            "words already there are not added twice");
}

void TestPostalCodeImport() {
    using tekito::userdata::PostalCodeImportError;
    const auto directory = std::filesystem::temp_directory_path() / L"tekito-postal-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    // Japan Post's rows: code in the third field, prefecture, city and town
    // in the seventh to ninth (the Japanese text as UTF-8 bytes).
    const std::string tokyo = "\xE6\x9D\xB1\xE4\xBA\xAC\xE9\x83\xBD";                   // Tokyo-to
    const std::string chiyoda = "\xE5\x8D\x83\xE4\xBB\xA3\xE7\x94\xB0\xE5\x8C\xBA";     // Chiyoda-ku
    const std::string town = "\xE5\x8D\x83\xE4\xBB\xA3\xE7\x94\xB0";                    // Chiyoda
    const std::string note = "\xEF\xBC\x88\xE6\xB3\xA8\xEF\xBC\x89";                    // (note), full-width
    std::string csv = "\xEF\xBB\xBF";
    for (int code = 1000000; code < 1060000; ++code) {
        csv += "13101,\"100  \",\"" + std::to_string(code) + "\",\"a\",\"b\",\"c\",\"" + tokyo + "\",\"" + chiyoda +
               "\",\"" + town + (code == 1000001 ? note : "") + "\",0,0,0,0,0,0\r\n";
    }
    PostalCodeImportError failure{};
    const auto built = tekito::userdata::BuildPostalCodePack(csv, L"2026.09", directory, failure);
    Require(built && built->count == 60000 && failure == PostalCodeImportError::None,
            "Japan Post's rows become the postal code pack");
    std::ifstream data(directory / L"zipcodes.tsv", std::ios::binary);
    std::string first, second;
    std::getline(data, first);
    std::getline(data, second);
    Require(first == "1000000\t" + tokyo + chiyoda + town && second == "1000001\t" + tokyo + chiyoda + town,
            "rows are code and address, in order, without notes in parentheses");
    data.close();
    const auto installed = tekito::userdata::InstalledPostalCodes(directory);
    Require(installed && installed->count == 60000 && installed->version == L"2026.09",
            "the added pack validates and says what it holds");

    Require(!tekito::userdata::BuildPostalCodePack("\x93\x8C\x8B\x9E", L"x", directory, failure) &&
                failure == PostalCodeImportError::ShiftJis,
            "Japan Post's Shift_JIS download is told apart");
    Require(!tekito::userdata::BuildPostalCodePack("a,b,c\n", L"x", directory, failure) &&
                failure == PostalCodeImportError::NotPostalCodes,
            "other files are not taken");
    Require(tekito::userdata::InstalledPostalCodes(directory).has_value(), "a failed import keeps what was there");

    Require(tekito::userdata::RemovePostalCodes(directory) && !tekito::userdata::InstalledPostalCodes(directory),
            "the postal codes can be removed");
    std::filesystem::remove_all(directory, error);
}

void TestDataPackValidation() {
    const auto dataRoot = tekito::ExternalLexiconProvider::DataPackRoot();
    for (const auto& pack : {L"standard-english", L"wikipedia-common-misspellings",
                             L"dictionary-display", L"frequency", L"phrase", L"slang",
                             L"wiktionary-slang", L"proper-nouns", L"pronunciation",
                             L"emoji", L"social-expression", L"japanese-phonetic",
                             L"qwerty-typo-catalog", L"special-conversions"}) {
        if (IsOptionalPack(pack) && !OptionalPackInstalled(pack)) continue;
        tekito::userdata::DataPackStatus status;
        const bool valid = tekito::userdata::ValidateDataPack(tekito::FindDataPack(dataRoot, pack), status);
        if (!valid) std::wcerr << L"[pack validation] " << pack << L": " << status.reason << L"\n";
        Require(valid &&
                    status.valid && !status.packId.empty() && !status.version.empty(),
                "data pack manifest, notice, index, and checksums validate");
    }

    const auto invalidRoot = std::filesystem::temp_directory_path() / L"tekito-invalid-pack";
    std::error_code error;
    std::filesystem::remove_all(invalidRoot, error);
    std::filesystem::create_directories(invalidRoot);
    std::ofstream(invalidRoot / L"NOTICE") << "invalid test pack\n";
    std::ofstream(invalidRoot / L"entries.tsv") << "word\n";
    std::ofstream(invalidRoot / L"entries.tsv.idx") << "index\n";
    std::ofstream(invalidRoot / L"manifest.json")
        << R"({"pack_id":"invalid","version":"0.0.0","language":"en-US",)"
           R"("type":"test","file":"entries.tsv","index_file":"entries.tsv.idx",)"
           R"("sha256":{"file":"00","index":"00"},"license":"TEST",)"
           R"("notice_file":"NOTICE"})";
    tekito::userdata::DataPackStatus invalidStatus;
    Require(!tekito::userdata::ValidateDataPack(invalidRoot, invalidStatus) &&
                !invalidStatus.valid,
            "checksum mismatch rejects an invalid optional data pack");
    std::filesystem::remove_all(invalidRoot, error);
}

class TestFrequencyProvider final : public tekito::IFrequencyProvider {
public:
    [[nodiscard]] double Score(std::wstring_view word) const noexcept override {
        ++calls;
        return word == L"neume" ? 10.0 : 1.0;
    }

    mutable int calls{0};
};

class TestPhraseProvider final : public tekito::IPhraseContextProvider {
public:
    [[nodiscard]] double Score(std::wstring_view word,
                               std::wstring_view context) const noexcept override {
        ++calls;
        return word == L"neume" && context == L"context" ? 5.0 : 0.0;
    }

    mutable int calls{0};
};

void TestRankingDataBoundaries() {
    const tekito::BuiltinLexiconProvider lexicon;
    TestFrequencyProvider frequency;
    TestPhraseProvider phrase;
    tekito::CandidateGenerator generator(lexicon, frequency, phrase);
    const auto candidates = generator.Generate(L"neme", L"context");
    Require(std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) {
                return candidate.text == L"neume";
            }) &&
                frequency.calls > 0 && phrase.calls > 0,
            "frequency and phrase providers are queried independently");

    const tekito::BuiltinLexiconEntry lowEntry{
        L"name", {}, tekito::SemanticLabel::Original, tekito::SemanticLabel::None, true, true};
    const tekito::BuiltinLexiconEntry highEntry{
        L"meme", {}, tekito::SemanticLabel::Original, tekito::SemanticLabel::None, true, true};
    std::vector<tekito::CorrectionCandidate> ranked{
        {1, false, false, false, false, false, lowEntry, L"name", {1.0, 0.0, 0.0}},
        {1, false, false, false, false, false, highEntry, L"meme", {10.0, 0.0, 0.0}},
    };
    const tekito::CandidateRanker ranker;
    ranker.RankCorrections(ranked, 4);
    Require(ranked.front().entry.raw == L"meme",
            "candidate ranker can consume independent frequency signals");

    std::vector<tekito::CorrectionCandidate> personalized{
        {1, false, false, false, false, false, lowEntry, L"neme", {1.0, 0.0, 0.0}},
        {1, false, false, false, false, false, highEntry, L"neme", {1.0, 0.0, 0.35}},
    };
    ranker.RankCorrections(personalized, 4);
    Require(personalized.front().entry.raw == L"meme",
            "candidate ranker applies a bounded learned preference among similar candidates");
}

void TestCompositeLexiconProvider() {
    tekito::UserDictionary dictionary;
    Require(dictionary.Add({1, L"ammotrack", L"AMMOTRACK", tekito::CandidatePolicyProtect}),
            "composite provider test accepts a user entry");

    const tekito::UserLexiconProvider userProvider(dictionary);
    const tekito::BuiltinLexiconProvider builtinProvider;
    const tekito::CompositeLexiconProvider provider(userProvider, builtinProvider);
    Require(provider.Entries().size() == userProvider.Entries().size() +
                                             builtinProvider.Entries().size(),
            "composite provider exposes primary and fallback entries");

    const tekito::CandidateGenerator generator(provider);
    const auto candidates = generator.Generate(L"ammotrack");
    Require(!candidates.empty() && candidates.front().text == L"AMMOTRACK",
            "user dictionary entry has priority over fallback lexicon");
    Require(HasFlag(candidates.front().sourceFlags,
                    tekito::CandidateSourceUserDictionary),
            "composite provider preserves user dictionary source metadata");
}

void TestPolicyEngineBoundary() {
    const tekito::PolicyEngine policy;
    const tekito::BuiltinLexiconEntry slang{
        L"sus", L"suspicious", tekito::SemanticLabel::Slang,
        tekito::SemanticLabel::Standard, true, false};
    Require(policy.RawFlags(slang) == tekito::CandidatePolicyProtect,
            "policy engine protects TEKITO-owned expressions");
    Require(policy.CandidateFlags(slang) ==
                (tekito::CandidatePolicyExpand | tekito::CandidatePolicySuggestOnly),
            "policy engine marks standard expansions as suggest-only");
}

void TestCandidateRankerBoundary() {
    const tekito::BuiltinLexiconEntry shortEntry{
        L"hello", {}, tekito::SemanticLabel::Original, tekito::SemanticLabel::None, true, true};
    const tekito::BuiltinLexiconEntry longEntry{
        L"environment", {}, tekito::SemanticLabel::Original, tekito::SemanticLabel::None, true, true};
    std::vector<tekito::CorrectionCandidate> candidates{
        {2, false, false, false, false, false, longEntry},
        {1, false, false, false, false, false, shortEntry},
    };

    const tekito::CandidateRanker ranker;
    ranker.RankCorrections(candidates, 7);
    Require(candidates.front().distance == 1,
            "candidate ranker prioritizes lower edit distance");
}

void TestUserDictionaryBoundary() {
    tekito::UserDictionary dictionary;
    Require(dictionary.Add({1, L"ammotrack", L"AMMOTRACK"}),
            "user dictionary accepts a valid entry");
    Require(!dictionary.Add({1, L"other", L"OTHER"}),
            "user dictionary rejects duplicate IDs");
    Require(!dictionary.Add({2, L"", L"EMPTY"}),
            "user dictionary rejects an empty raw key");
    Require(dictionary.Find(1) != nullptr && dictionary.Find(1)->candidate == L"AMMOTRACK",
            "user dictionary finds an entry by ID");
    Require(dictionary.Entries().size() == 1,
            "user dictionary exposes only accepted entries");
    Require(dictionary.Remove(1), "user dictionary removes an entry");
    Require(dictionary.Find(1) == nullptr && dictionary.Entries().empty(),
            "user dictionary is empty after removal");
}

void TestUserLexiconProviderBoundary() {
    tekito::UserDictionary dictionary;
    Require(dictionary.Add({1, L"ammotrack", L"AMMOTRACK", tekito::CandidatePolicyProtect}),
            "user lexicon test accepts a dictionary entry");

    const tekito::UserLexiconProvider provider(dictionary);
    Require(provider.Entries().size() == 1,
            "user lexicon provider exposes enabled entries");
    Require(HasFlag(provider.Entries().front().sourceFlags,
                    tekito::CandidateSourceUserDictionary),
            "user lexicon provider marks its source");

    const tekito::CandidateGenerator generator(provider);
    const auto candidates = generator.Generate(L"ammotrack");
    Require(!candidates.empty() && candidates.front().text == L"AMMOTRACK",
            "user dictionary candidate is generated first");
    Require(HasFlag(candidates.front().sourceFlags,
                    tekito::CandidateSourceUserDictionary),
            "user dictionary candidate preserves its source metadata");
    Require(candidates.front().dictionaryEntryId == L"user:ammotrack",
            "user dictionary candidate has a user entry ID");

    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"ammotrack", candidates);
    const auto boundary = state.OnSpace();
    Require(boundary.text == L"ammotrack " && !boundary.showCandidates &&
                state.State() == tekito::CompositionState::Boundary,
            "User Dictionary protect policy keeps raw text at the first Space boundary");

    tekito::UserDictionary correctionDictionary;
    Require(correctionDictionary.Add({2, L"neme", L"name", tekito::CandidatePolicyCorrect}),
            "User Dictionary correct policy accepts a correction entry");
    const tekito::UserLexiconProvider correctionProvider(correctionDictionary);
    const tekito::CandidateGenerator correctionGenerator(correctionProvider);
    state.BeginOrUpdate(L"neme", correctionGenerator.Generate(L"neme"));
    const auto correctionBoundary = state.OnSpace();
    Require(correctionBoundary.text == L"name " && state.IsActive(),
            "User Dictionary correct policy accepts its candidate at Space");
}

void TestUserDictionaryEngineConnection() {
    const auto path = std::filesystem::temp_directory_path() /
                      L"tekito-user-dictionary-engine-test.db";
    std::error_code error;
    std::filesystem::remove(path, error);

    tekito::UserDictionary source;
    Require(source.Add({1, L"ammotrack", L"AMMOTRACK",
                         tekito::CandidatePolicyProtect, true, true}),
            "engine connection test accepts a user dictionary entry");

    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "engine connection test opens SQLite");
        Require(repository.Save(source), "engine connection test saves SQLite data");
    }

    tekito::UserDictionary dictionary;
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "engine connection test reopens SQLite");
        Require(repository.Load(dictionary), "engine connection test loads SQLite data");
    }

    const auto engine = tekito::CreateDefaultConversionEngine(dictionary);
    Require(engine != nullptr, "default engine factory accepts a user dictionary");
    const auto result = engine->Convert({L"ammotrack"});
    Require(!result.candidates.empty() && result.candidates.front().text == L"AMMOTRACK",
            "loaded user dictionary candidate is generated first by the default engine");
    Require(HasFlag(result.candidates.front().sourceFlags,
                    tekito::CandidateSourceUserDictionary),
            "default engine preserves user dictionary source metadata");
    Require(result.candidates.front().dictionaryEntryId == L"user:ammotrack",
            "default engine preserves the user dictionary entry ID");

    std::filesystem::remove(path, error);
}

void TestUserDictionaryRefreshInPlace() {
    // The TSF edits the dictionary it gave the engine and asks the engine to
    // refresh, instead of rebuilding it (which reloads every Data Pack).
    tekito::UserDictionary dictionary;
    const auto engine = tekito::CreateDefaultConversionEngine(dictionary);
    Require(engine != nullptr, "refresh test creates the default engine");
    const auto before = engine->Convert({L"cpta"});
    Require(before.candidates.empty() || before.candidates.front().text != L"Capitata",
            "refresh test starts without the user entry");

    tekito::UserDictionary updated;
    Require(updated.Add({1, L"cpta", L"Capitata", tekito::CandidatePolicyCorrect, false, true}),
            "refresh test accepts a user dictionary entry");
    dictionary = std::move(updated);
    engine->RefreshUserDictionary();
    const auto added = engine->Convert({L"cpta"});
    Require(!added.candidates.empty() && added.candidates.front().text == L"Capitata",
            "refreshed engine offers a user entry added after it was created");

    dictionary = {};
    engine->RefreshUserDictionary();
    const auto removed = engine->Convert({L"cpta"});
    Require(std::none_of(removed.candidates.begin(), removed.candidates.end(),
                         [](const tekito::Candidate& candidate) {
                             return candidate.text == L"Capitata";
                         }),
            "refreshed engine drops a user entry removed after it was created");
}

void TestUndoneCorrectionIsOnlyOffered() {
    // Undoing an automatic correction (Backspace) keeps the word as typed
    // from then on; the correction is still in the list.
    tekito::UserDictionary dictionary;
    tekito::UserLearningStore learning;
    tekito::UserLearningProvider provider(learning);
    const auto engine = tekito::CreateDefaultConversionEngine(dictionary, provider);
    tekito::InputStateMachine state;
    state.BeginOrUpdate(L"teh", engine->Convert({L"teh"}).candidates);
    Require(state.OnSpace().text == L"the ", "undo test starts with teh corrected at Space");
    state.Reset();

    learning.RecordExposure(L"teh", L"the");
    learning.RecordUndo(L"teh", L"the");
    const auto after = engine->Convert({L"teh"}).candidates;
    state.BeginOrUpdate(L"teh", after);
    Require(state.OnSpace().text == L"teh ", "an undone correction is not applied again at Space");
    Require(std::any_of(after.begin(), after.end(), [](const auto& candidate) { return candidate.text == L"the"; }),
            "an undone correction is still offered");
}

void TestSqliteUserDictionaryRepository() {
    const auto path = std::filesystem::temp_directory_path() /
                      L"tekito-user-dictionary-repository-test.db";
    std::error_code error;
    std::filesystem::remove(path, error);

    tekito::UserDictionary source;
    Require(source.Add({1, L"ammotrack", L"AMMOTRACK", tekito::CandidatePolicyProtect,
                        true, true}),
            "sqlite test accepts enabled user entry");
    Require(source.Add({2, L"draft", L"Draft", tekito::CandidatePolicySuggestOnly,
                        false, false}),
            "sqlite test accepts disabled user entry");

    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "sqlite repository opens and creates its schema");
        Require(repository.IsOpen(), "sqlite repository reports open state");
        Require(repository.Save(source), "sqlite repository saves user dictionary entries");
    }

    tekito::UserDictionary loaded;
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "sqlite repository reopens its database");
        Require(repository.Load(loaded), "sqlite repository loads user dictionary entries");
    }
    Require(loaded.Entries().size() == 2, "sqlite repository restores all entries");
    const auto* enabled = loaded.Find(1);
    const auto* disabled = loaded.Find(2);
    Require(enabled && enabled->candidate == L"AMMOTRACK" && enabled->caseSensitive,
            "sqlite repository restores candidate text and case sensitivity");
    Require(disabled && !disabled->enabled &&
                HasFlag(disabled->policyFlags, tekito::CandidatePolicySuggestOnly),
            "sqlite repository restores disabled state and policy flags");

    std::filesystem::remove(path, error);
}

void TestSqliteUserSettingsRepository() {
    const auto path = std::filesystem::temp_directory_path() /
                      L"tekito-user-settings-repository-test.db";
    std::error_code error;
    std::filesystem::remove(path, error);

    tekito::userdata::UserSettings source;
    source.defaultInputMode = tekito::InputMode::Direct;
    source.lastInputMode = tekito::InputMode::Direct;
    source.restoreLastInputMode = false;
    source.learningEnabled = false;
    source.correctionEnabled = false;
    source.commonMisspellingsEnabled = false;
    source.contextSuggestionsEnabled = false;
    source.completionEnabled = false;
    source.candidateWindowEnabled = false;
    source.japanesePhoneticSuggestionsEnabled = false;
    source.socialExpressionRange = 4;
    source.socialPersonalization = 2;
    source.candidateWindowStyle = 1;
    source.toggleKey = 3;
    source.periodOnEnter = true;
    source.uiLanguage = 2;
    source.keyboardType = 1;
    source.lastJapaneseProfileMode = tekito::InputMode::Convert;
    source.japaneseProfileEnglishMode = tekito::InputMode::Direct;
    source.japaneseSpaceWidth = 2;
    source.japanesePunctuation = 3;
    source.japaneseDigitWidth = 1;
    source.japaneseSymbolWidth = 1;
    source.advancedSettings = true;
    source.japanesePredictionEnabled = false;
    source.candidateRows = 7;
    source.meaningsEnabled = false;
    source.japaneseSwitchOrder = 2;
    source.japaneseEnabled = false;
    source.modeIndicatorEnabled = false;
    source.dateConversion = false;
    source.numberConversion = false;
    source.symbolConversion = false;
    source.calculatorEnabled = false;
    source.excludedApps = {L"Code.exe", L"code.exe", L"game.exe"};
    {
        const tekito::userdata::UserSettings defaults;
        Require(defaults.lastJapaneseProfileMode == tekito::InputMode::Japanese &&
                    defaults.japaneseProfileEnglishMode == tekito::InputMode::Convert &&
                    defaults.keyboardType == 0,
                "the Japanese profile starts in Japanese, with Auto as its English mode");
    }
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "settings repository opens its database");
        Require(repository.SaveSettings(source), "settings repository saves input mode settings");
    }

    tekito::userdata::UserSettings loaded;
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "settings repository reopens its database");
        Require(repository.LoadSettings(loaded), "settings repository loads input mode settings");
    }
    Require(loaded.defaultInputMode == tekito::InputMode::Direct &&
                loaded.lastInputMode == tekito::InputMode::Direct &&
                !loaded.restoreLastInputMode,
            "settings repository restores all input mode settings");
    Require(!loaded.learningEnabled && !loaded.correctionEnabled &&
                !loaded.commonMisspellingsEnabled &&
                !loaded.contextSuggestionsEnabled && !loaded.completionEnabled &&
                !loaded.candidateWindowEnabled && !loaded.japanesePhoneticSuggestionsEnabled &&
                loaded.socialExpressionRange == 4 && loaded.socialPersonalization == 2,
            "settings repository restores Input and Learning settings");
    Require(loaded.candidateWindowStyle == 1 && loaded.toggleKey == 3 && loaded.periodOnEnter &&
                loaded.uiLanguage == 2,
            "settings repository restores the candidate style and toggle key");
    Require(loaded.keyboardType == 1 &&
                loaded.lastJapaneseProfileMode == tekito::InputMode::Convert &&
                loaded.japaneseProfileEnglishMode == tekito::InputMode::Direct &&
                loaded.japaneseSpaceWidth == 2 && loaded.japanesePunctuation == 3 && loaded.japaneseDigitWidth == 1 && loaded.japaneseSymbolWidth == 1 && loaded.advancedSettings &&
                !loaded.japanesePredictionEnabled &&
                loaded.candidateRows == 7 && !loaded.meaningsEnabled && loaded.japaneseSwitchOrder == 2 &&
                    !loaded.japaneseEnabled && !loaded.modeIndicatorEnabled,
            "settings repository restores the keyboard and Japanese settings");
    Require(!loaded.dateConversion && !loaded.numberConversion && !loaded.symbolConversion &&
                !loaded.calculatorEnabled,
            "settings repository restores the special conversions");
    Require(loaded.excludedApps.size() == 2 && loaded.excludedApps[0] == L"Code.exe" &&
                loaded.excludedApps[1] == L"game.exe",
            "settings repository keeps excluded apps once each, ignoring case");

    const std::vector<tekito::japanese::UserWord> sourceWords{
        {L"なかの", L"中埜", tekito::japanese::UserWordKind::Surname},
        {L"てきとう", L"TEKITO", tekito::japanese::UserWordKind::ProperNoun},
        {L"かお", L"(^^)", tekito::japanese::UserWordKind::Symbol, tekito::japanese::UserWordAction::Suggest},
        {L"きしゃ", L"汽車", tekito::japanese::UserWordKind::Noun, tekito::japanese::UserWordAction::Suppress}};
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "Japanese user words repository opens its database");
        Require(repository.SaveJapaneseUserWords(sourceWords), "Japanese user words are saved");
    }
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        std::vector<tekito::japanese::UserWord> loadedWords;
        Require(repository.Open() && repository.LoadJapaneseUserWords(loadedWords),
                "Japanese user words load");
        Require(loadedWords == sourceWords, "Japanese user words come back as saved, in order");
        Require(repository.SaveJapaneseUserWords({}) && repository.LoadJapaneseUserWords(loadedWords) &&
                    loadedWords.empty(),
                "saving no words empties the table");
    }
    {
        // A table from before the action column gets it, words given first.
        const auto oldPath = std::filesystem::temp_directory_path() / L"tekito-old-japanese-words.db";
        std::filesystem::remove(oldPath);
        sqlite3* database = nullptr;
        Require(sqlite3_open16(oldPath.c_str(), &database) == SQLITE_OK &&
                    sqlite3_exec(database,
                                 "CREATE TABLE japanese_user_words (reading TEXT NOT NULL, surface TEXT NOT NULL, "
                                 "kind TEXT NOT NULL, PRIMARY KEY (reading, surface));"
                                 "INSERT INTO japanese_user_words VALUES ('\xE3\x81\xAA', '\xE8\x8F\x9C', 'noun');",
                                 nullptr, nullptr, nullptr) == SQLITE_OK,
                "an old Japanese words table is made");
        sqlite3_close(database);
        std::vector<tekito::japanese::UserWord> migrated;
        {
            tekito::userdata::SqliteUserDictionaryRepository repository(oldPath);
            Require(repository.Open() && repository.LoadJapaneseUserWords(migrated),
                    "an old Japanese words table opens");
        }
        Require(migrated.size() == 1 && migrated.front().surface == L"菜" &&
                    migrated.front().action == tekito::japanese::UserWordAction::First,
                "its words are given first");
        std::filesystem::remove(oldPath);
    }

    tekito::SocialLearningStore sourceSocial;
    sourceSocial.RecordExposure(L"lol", L"😂");
    sourceSocial.RecordSelection(L"lol", L"😂");
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "social learning repository opens its database");
        Require(repository.SaveSocialLearning(sourceSocial), "social learning repository saves events");
    }
    tekito::SocialLearningStore loadedSocial;
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "social learning repository reopens its database");
        Require(repository.LoadSocialLearning(loadedSocial), "social learning repository loads events");
    }
    Require(loadedSocial.Entries().size() == 1 && loadedSocial.Entries()[0].exposure == 1,
            "social learning roundtrip keeps local event data");

    tekito::japanese::JapaneseLearningStore sourceJapanese;
    sourceJapanese.RecordChoice(L"きかい", L"機械", L"機会");
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "Japanese learning repository opens its database");
        Require(repository.SaveJapaneseLearning(sourceJapanese), "Japanese learning saves");
    }
    tekito::japanese::JapaneseLearningStore loadedJapanese;
    {
        tekito::userdata::SqliteUserDictionaryRepository repository(path);
        Require(repository.Open(), "Japanese learning repository reopens its database");
        Require(repository.LoadJapaneseLearning(loadedJapanese), "Japanese learning loads");
        Require(loadedJapanese.Preference(L"きかい", L"機械") == 1.0,
                "Japanese learning keeps what was chosen");
        Require(repository.ResetJapaneseLearning(), "Japanese learning resets");
        Require(repository.LoadJapaneseLearning(loadedJapanese) && loadedJapanese.Empty(),
                "resetting Japanese learning empties it");
    }

    std::filesystem::remove(path, error);
}

void TestRuntimeModeState() {
    constexpr wchar_t mappingName[] = L"Local\\TEKITO.RuntimeState.CoreTests.V3";
    tekito::userdata::RuntimeModeState first(tekito::InputMode::Convert, mappingName);
    tekito::userdata::RuntimeModeState second(tekito::InputMode::Convert, mappingName);
    Require(first.IsAvailable() && second.IsAvailable(),
            "runtime mode state opens a shared local mapping");
    Require(second.JapaneseMode() == tekito::InputMode::Japanese,
            "the Japanese profile starts in Japanese");
    const auto modeGeneration = second.ModeGeneration();
    first.SetMode(tekito::InputMode::Direct);
    Require(second.Mode() == tekito::InputMode::Direct &&
                second.ModeGeneration() > modeGeneration,
            "runtime mode changes are immediately visible across clients");
    const auto japaneseGeneration = second.ModeGeneration();
    first.SetJapaneseMode(tekito::InputMode::Convert);
    Require(second.JapaneseMode() == tekito::InputMode::Convert &&
                second.Mode() == tekito::InputMode::Direct &&
                second.ModeGeneration() > japaneseGeneration,
            "the Japanese profile's mode is shared on its own");
    const auto settingsGeneration = second.SettingsGeneration();
    first.NotifySettingsChanged();
    Require(second.SettingsGeneration() > settingsGeneration,
            "runtime settings notification is visible across clients");
    const auto dictionaryGeneration = second.DictionaryGeneration();
    first.NotifyDictionaryChanged();
    Require(second.DictionaryGeneration() > dictionaryGeneration,
            "runtime dictionary notification is visible across clients");
    const auto learningGeneration = second.LearningGeneration();
    first.NotifyLearningChanged();
    Require(second.LearningGeneration() > learningGeneration,
            "runtime learning notification is visible across clients");
}

}  // namespace

int main() {
    TestFirstSpaceAndLiveCycle();
    TestSpaceBoundaryAndAutoApplyPolicy();
    TestSentenceBoundaryAndCapitalizationProvenance();
    TestSpaceCyclesAndPreservesCandidateSelection();
    TestEngineAndAutoApplyPolicyIntegration();
    TestPagingAndLoop();
    TestFixedPageSize();
    TestShiftSpaceAndRestore();
    TestTabAndShiftTabCandidateSelection();
    TestSemanticCandidateActions();
    TestInputModeDirectPassThrough();
    TestEnterCandidateNavigation();
    TestCandidateClickAndArrowNavigationState();
    TestNoSkippedCandidatesAcrossFullCycle();
    TestPrintableAfterCyclingStartsCleanNextSession();
    TestPunctuationCommitsCurrentCandidateWithoutSpace();
    TestCancelRestoresRawInput();
    TestConversionEngineBoundary();
    TestCandidateEngine();
    TestBuiltinLexiconData();
    TestBuiltinLexiconProvider();
    TestExternalLexiconProvider();
    TestWikipediaCommonMisspellingsProvider();
    TestInstalledDataPacks();
    TestChatAbbreviationsStayAsTyped();
    TestTypoModelBoundary();
    TestTargetTextCandidateDiagnostics();
    TestDictionaryServiceBoundary();
    TestLocalDataPackProviders();
    TestUserDictionaryImportExport();
    TestUserLearningStore();
    TestSqliteUserLearningRepository();
    TestDataPackValidation();
    TestPostalCodeImport();
    TestImeDictionaryImport();
    TestEnglishWritingRules();
    TestEnglishWritingOptions();
    TestRankingDataBoundaries();
    TestCompositeLexiconProvider();
    TestPolicyEngineBoundary();
    TestCandidateRankerBoundary();
    TestUserDictionaryBoundary();
    TestUserLexiconProviderBoundary();
    TestUserDictionaryEngineConnection();
    TestUserDictionaryRefreshInPlace();
    TestUndoneCorrectionIsOnlyOffered();
    TestSqliteUserDictionaryRepository();
    TestSqliteUserSettingsRepository();
    TestRuntimeModeState();
    std::cout << "All TEKITO core tests passed.\n";
    return 0;
}
