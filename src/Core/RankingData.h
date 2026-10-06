#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {

struct RankingSignals {
    double frequency{0.0};
    double phraseContext{0.0};
    double userLearning{0.0};
    double contextConfidence{0.0};
};

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
    // The words seen right after `context` that start with `prefix`, the
    // most common first.
    [[nodiscard]] virtual std::vector<std::wstring> Following(std::wstring_view /*context*/,
                                                              std::wstring_view /*prefix*/,
                                                              std::size_t /*limit*/) const {
        return {};
    }
};

class IUserLearningProvider {
public:
    virtual ~IUserLearningProvider() = default;
    [[nodiscard]] virtual double Score(std::wstring_view word) const noexcept = 0;
    [[nodiscard]] virtual double Score(std::wstring_view,
                                       std::wstring_view candidate) const noexcept {
        return Score(candidate);
    }
    // Whether the user undid `candidate` as the correction of `rawText`
    // more than they took it: it is then no longer applied on its own.
    [[nodiscard]] virtual bool Undone(std::wstring_view /*rawText*/,
                                      std::wstring_view /*candidate*/) const noexcept {
        return false;
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
