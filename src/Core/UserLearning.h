#pragma once

#include "Core/RankingData.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tekito {

struct UserLearningEntry {
    std::wstring word;
    std::uint64_t candidateSelections{0};
    std::uint64_t rawKeeps{0};
    std::uint64_t undoCount{0};
};

struct UserLearningPreference {
    std::wstring rawText;
    std::wstring candidate;
    double alpha{0.0};
    double beta{0.0};
    std::uint64_t exposure{0};
    double directEventMass{0.0};
};

class UserLearningStore final {
public:
    [[nodiscard]] bool Add(UserLearningEntry entry);
    void RecordCandidateSelection(std::wstring_view word);
    void RecordRawKeep(std::wstring_view word);
    void RecordUndo(std::wstring_view word);
    [[nodiscard]] bool AddPreference(UserLearningPreference preference);
    void RecordExposure(std::wstring_view rawText, std::wstring_view candidate);
    void RecordSelection(std::wstring_view rawText, std::wstring_view candidate);
    void RecordAutomaticAcceptance(std::wstring_view rawText,
                                   std::wstring_view candidate);
    void RecordNonSelection(std::wstring_view rawText, std::wstring_view candidate);
    void RecordUndo(std::wstring_view rawText, std::wstring_view candidate);
    [[nodiscard]] double Score(std::wstring_view word) const noexcept;
    [[nodiscard]] double Score(std::wstring_view rawText,
                               std::wstring_view candidate) const noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::span<const UserLearningEntry> Entries() const noexcept;
    [[nodiscard]] std::span<const UserLearningPreference> Preferences() const noexcept;
    [[nodiscard]] std::size_t PreferenceCount() const noexcept { return preferences_.size(); }

private:
    UserLearningEntry& FindOrAdd(std::wstring_view word);
    UserLearningPreference& FindOrAddPreference(std::wstring_view rawText,
                                                std::wstring_view candidate);
    [[nodiscard]] const UserLearningPreference* FindPreference(
        std::wstring_view rawText, std::wstring_view candidate) const noexcept;
    std::vector<UserLearningEntry> entries_;
    std::vector<UserLearningPreference> preferences_;
};

class UserLearningProvider final : public IUserLearningProvider {
public:
    explicit UserLearningProvider(const UserLearningStore& store) noexcept : store_(store) {}
    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] double Score(std::wstring_view word) const noexcept override {
        return enabled_ ? store_.Score(word) : 0.0;
    }
    [[nodiscard]] double Score(std::wstring_view rawText,
                               std::wstring_view candidate) const noexcept override {
        return enabled_ ? store_.Score(rawText, candidate) : 0.0;
    }

private:
    const UserLearningStore& store_;
    bool enabled_{true};
};

struct SocialLearningEntry {
    std::wstring trigger;
    std::wstring candidate;
    double alpha{0.0};
    double beta{0.0};
    std::uint64_t exposure{0};
    double directEventMass{0.0};
};

class SocialLearningStore final {
public:
    [[nodiscard]] bool Add(SocialLearningEntry entry);
    void RecordExposure(std::wstring_view trigger, std::wstring_view candidate);
    void RecordSelection(std::wstring_view trigger, std::wstring_view candidate);
    void RecordNonSelection(std::wstring_view trigger, std::wstring_view candidate);
    void RecordUndo(std::wstring_view trigger, std::wstring_view candidate);
    [[nodiscard]] double Score(std::wstring_view trigger,
                               std::wstring_view candidate) const noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::span<const SocialLearningEntry> Entries() const noexcept;

private:
    SocialLearningEntry& FindOrAdd(std::wstring_view trigger, std::wstring_view candidate);
    [[nodiscard]] const SocialLearningEntry* Find(std::wstring_view trigger,
                                                  std::wstring_view candidate) const noexcept;
    std::vector<SocialLearningEntry> entries_;
};

class SocialLearningProvider final {
public:
    explicit SocialLearningProvider(const SocialLearningStore& store) noexcept : store_(store) {}
    void SetLevel(int level) noexcept { level_ = level; }
    void SetProfile(int range, int personalization) noexcept {
        level_ = personalization <= 0 ? 0 : std::clamp(range, 0, 4);
    }
    [[nodiscard]] double Score(std::wstring_view trigger,
                               std::wstring_view candidate) const noexcept;

private:
    const SocialLearningStore& store_;
    int level_{1};
};

}  // namespace tekito
