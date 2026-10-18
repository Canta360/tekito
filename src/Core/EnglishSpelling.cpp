#include "Core/EnglishSpelling.h"

#include "Core/ConversionEngine.h"
#include "Core/InputStateMachine.h"

#include <cwctype>

namespace tekito {

namespace {

bool SameIgnoringCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

}  // namespace

EnglishSpelling::Result EnglishSpelling::Spell(std::wstring_view typed, std::wstring_view before) const {
    Result result;
    result.word = std::wstring(typed);
    ConversionRequest request;
    request.rawText = std::wstring(typed);
    // Mix puts the capital at a sentence start itself.
    request.context.sentenceStart = false;
    request.context.precedingText = std::wstring(before);
    request.capitalization.origin = CapitalizationOrigin::UserTyped;
    request.options.spelling = variety_;
    auto candidates = engine_.Convert(request).candidates;
    for (const auto& candidate : candidates) {
        if (candidate.text.empty() || SameIgnoringCase(candidate.text, typed)) continue;
        if (result.others.size() < 8) result.others.push_back(candidate.text);
    }
    // Space decides as it does in English Auto.
    InputStateMachine state;
    state.SetInputMode(InputMode::Convert);
    state.BeginOrUpdate(std::wstring(typed), std::move(candidates));
    const auto action = state.OnSpace();
    if (action.kind != ActionKind::ReplaceComposition || !action.showCandidates) return result;
    std::wstring word = action.text;
    while (!word.empty() && word.back() == L' ') word.pop_back();
    if (word.empty() || SameIgnoringCase(word, typed)) return result;
    std::erase(result.others, word);
    result.word = std::move(word);
    result.corrected = true;
    return result;
}

}  // namespace tekito
