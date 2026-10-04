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

// The user's settings on `composer`; `learning` is used only while learning
// is on.
inline void ApplyJapaneseSettings(japanese::JapaneseComposer& composer, const UserSettings& settings,
                                  japanese::JapaneseLearningStore* learning) {
    composer.SetPunctuationStyle(
        static_cast<japanese::PunctuationStyle>(std::clamp(settings.japanesePunctuation, 0, 3)));
    composer.SetHalfWidthDigits(settings.japaneseDigitWidth == 0);
    composer.SetHalfWidthSymbols(settings.japaneseSymbolWidth == 1);
    composer.SetLearning(settings.learningEnabled ? learning : nullptr);
    composer.SetPredictionEnabled(settings.japanesePredictionEnabled);
    composer.SetLiveConversion(settings.japaneseLiveConversion);
    composer.SetSpecialConversions(&SpecialConversions::Installed(), SpecialOptionsFor(settings));
}

}  // namespace tekito::userdata
