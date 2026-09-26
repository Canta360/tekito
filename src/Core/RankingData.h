#pragma once

#include <string_view>

namespace tekito {

struct RankingSignals {
    double frequency{0.0};
    double phraseContext{0.0};
    double userLearning{0.0};
    double contextConfidence{0.0};
};

constexpr double kStrongContextConfidence = 5.0;

class IFrequencyProvider {
public:
    virtual ~IFrequencyProvider() = default;
    [[nodiscard]] virtual double Score(std::wstring_view word) const noexcept = 0;
};

class IPhraseContextProvider {
public:
    virtual ~IPhraseContextProvider() = default;
    [[nodiscard]] virtual double Score(std::wstring_view word,
                                       std::wstring_view context) const noexcept = 0;
};

class IUserLearningProvider {
public:
    virtual ~IUserLearningProvider() = default;
    [[nodiscard]] virtual double Score(std::wstring_view word) const noexcept = 0;
    [[nodiscard]] virtual double Score(std::wstring_view,
                                       std::wstring_view candidate) const noexcept {
        return Score(candidate);
    }
};

class NullFrequencyProvider final : public IFrequencyProvider {
public:
    [[nodiscard]] double Score(std::wstring_view) const noexcept override { return 0.0; }
};

class NullPhraseContextProvider final : public IPhraseContextProvider {
public:
    [[nodiscard]] double Score(std::wstring_view,
                               std::wstring_view) const noexcept override {
        return 0.0;
    }
};

class NullUserLearningProvider final : public IUserLearningProvider {
public:
    [[nodiscard]] double Score(std::wstring_view) const noexcept override { return 0.0; }
};

}  // namespace tekito
