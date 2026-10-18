#pragma once

#include "Core/Japanese/JapaneseComposer.h"
#include "Core/Japanese/JapaneseKeys.h"
#include "Core/SpecialConversions.h"
#include "UserData/UserSettings.h"

#include <algorithm>
#include <cstddef>

namespace tekito::japanese {
class JapaneseLearningStore;
}

namespace tekito::userdata {

// The user's settings as Japanese typing takes them, shared by the text
// service and the testbed so both type alike.

// The special conversions the user turned on.
[[nodiscard]] inline SpecialConversionOptions SpecialOptionsFor(const UserSettings& settings) noexcept {
    return {settings.dateConversion, settings.numberConversion, settings.symbolConversion,
            settings.calculatorEnabled};
}

// Rows a page of the Japanese candidate list shows.
[[nodiscard]] inline std::size_t JapanesePageSize(const UserSettings& settings) noexcept {
    const auto rows = static_cast<std::size_t>(settings.candidateRows);
    return rows ? rows : 9;
}

// What keys do that depends on the settings (japanese::TranslateKey).
[[nodiscard]] inline japanese::KeyOptions JapaneseKeyOptions(const UserSettings& settings) noexcept {
    japanese::KeyOptions options;
    options.fullWidthSpace = settings.japaneseSpaceWidth != 1;
    options.pageSize = JapanesePageSize(settings);
    return options;
}

// What Japanese mode and Mix each choose for themselves (`mixed`: Mix's).
inline void ApplyModeSettings(japanese::JapaneseComposer& composer, const UserSettings& settings, bool mixed) {
    composer.SetPredictionEnabled(mixed ? settings.mixPredictionEnabled : settings.japanesePredictionEnabled);
    composer.SetEmojiOnFirstPage(mixed ? settings.mixFirstPageSymbol : settings.japaneseFirstPageSymbol);
}

// The user's settings on `composer`; `learning` is used only while learning
// is on. `mixed`: for Mix (ApplyModeSettings).
inline void ApplyJapaneseSettings(japanese::JapaneseComposer& composer, const UserSettings& settings,
                                  japanese::JapaneseLearningStore* learning, bool mixed = false) {
    composer.SetPunctuationStyle(
        static_cast<japanese::PunctuationStyle>(std::clamp(settings.japanesePunctuation, 0, 3)));
    composer.SetHalfWidthDigits(settings.japaneseDigitWidth != 1);
    composer.SetSingleDigitFullWidth(settings.japaneseDigitWidth == 2);
    composer.SetKanjiRange(static_cast<japanese::KanjiRange>(std::clamp(settings.japaneseKanjiRange, 0, 3)),
                           std::clamp(settings.japaneseKanjiGrade, 1, 6));
    composer.SetRoughNumbersInKanji(settings.japaneseRoughNumbersKanji);
    composer.SetNumberStyle(static_cast<japanese::JapaneseComposer::NumberStyle>(std::clamp(settings.japaneseNumberStyle, 0, 5)));
    composer.SetHalfWidthSymbols(settings.japaneseSymbolWidth == 1);
    composer.SetHalfWidthLetters(settings.japaneseLetterWidth == 1);
    composer.SetLearning(settings.learningEnabled ? learning : nullptr);
    composer.SetLiveConversion(settings.japaneseLiveConversion);
    composer.SetSentenceCheck(settings.japaneseSentenceCheck);
    composer.SetTypedForms(settings.japaneseTypedForms);
    composer.SetSingleVowelKana(settings.mixSingleVowelKana);
    composer.SetLiveRefine(settings.japaneseLiveRefine);
    composer.SetMixLiveConversion(settings.mixLiveConversion);
    composer.SetSpecialConversions(&SpecialConversions::Installed(), SpecialOptionsFor(settings));
    ApplyModeSettings(composer, settings, mixed);
}

}  // namespace tekito::userdata
