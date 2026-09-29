#include "Core/UserLearning.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <utility>

namespace tekito {

namespace {

constexpr double kMaximumLearningMass = 1000000.0;
constexpr double kUndoMass = 1.5;

std::wstring LearningKey(std::wstring_view value) {
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

void AddMass(double& value, double amount) noexcept {
    value = std::min(kMaximumLearningMass, value + amount);
}

void AddCount(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

bool ValidMass(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= kMaximumLearningMass;
}

}  // namespace

bool UserLearningStore::Add(UserLearningEntry entry) {
    if (entry.word.empty()) return false;
    const auto position = std::lower_bound(
        entries_.begin(), entries_.end(), entry.word,
        [](const auto& existing, std::wstring_view word) { return existing.word < word; });
    if (position != entries_.end() && position->word == entry.word) return false;
    entries_.insert(position, std::move(entry));
    return true;
}

UserLearningEntry& UserLearningStore::FindOrAdd(std::wstring_view word) {
    const auto position = std::lower_bound(
        entries_.begin(), entries_.end(), word,
        [](const auto& entry, std::wstring_view value) { return entry.word < value; });
    if (position != entries_.end() && position->word == word) return *position;
    return *entries_.insert(position, {std::wstring(word)});
}

void UserLearningStore::RecordCandidateSelection(std::wstring_view word) {
    if (!word.empty()) ++FindOrAdd(word).candidateSelections;
}

void UserLearningStore::RecordRawKeep(std::wstring_view word) {
    if (!word.empty()) ++FindOrAdd(word).rawKeeps;
}

void UserLearningStore::RecordUndo(std::wstring_view word) {
    if (!word.empty()) ++FindOrAdd(word).undoCount;
}

double UserLearningStore::Score(std::wstring_view word) const noexcept {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), word,
        [](const auto& entry, std::wstring_view value) { return entry.word < value; });
    if (it == entries_.end()) return 0.0;
    if (it->word != word) return 0.0;
    return static_cast<double>(it->candidateSelections) -
           static_cast<double>(it->undoCount) * 0.5;
}

bool UserLearningStore::AddPreference(UserLearningPreference preference) {
    if (preference.rawText.empty() || preference.candidate.empty() ||
        !ValidMass(preference.alpha) || !ValidMass(preference.beta) ||
        !ValidMass(preference.directEventMass)) {
        return false;
    }
    preference.rawText = LearningKey(preference.rawText);
    preference.candidate = LearningKey(preference.candidate);
    if (preference.rawText.empty() || preference.candidate.empty()) return false;

    const auto position = std::lower_bound(
        preferences_.begin(), preferences_.end(), preference,
        [](const auto& existing, const auto& value) {
            return std::pair{existing.rawText, existing.candidate} <
                   std::pair{value.rawText, value.candidate};
        });
    if (position != preferences_.end() && position->rawText == preference.rawText &&
        position->candidate == preference.candidate) {
        return false;
    }
    preferences_.insert(position, std::move(preference));
    return true;
}

UserLearningPreference& UserLearningStore::FindOrAddPreference(
    std::wstring_view rawText, std::wstring_view candidate) {
    const auto normalizedRaw = LearningKey(rawText);
    const auto normalizedCandidate = LearningKey(candidate);
    const auto position = std::lower_bound(
        preferences_.begin(), preferences_.end(),
        std::pair{std::wstring_view(normalizedRaw), std::wstring_view(normalizedCandidate)},
        [](const auto& existing, const auto& value) {
            return std::pair<std::wstring_view, std::wstring_view>{existing.rawText,
                                                                    existing.candidate} < value;
        });
    if (position != preferences_.end() && position->rawText == normalizedRaw &&
        position->candidate == normalizedCandidate) {
        return *position;
    }
    return *preferences_.insert(position, {normalizedRaw, normalizedCandidate});
}

const UserLearningPreference* UserLearningStore::FindPreference(
    std::wstring_view rawText, std::wstring_view candidate) const noexcept {
    const auto normalizedRaw = LearningKey(rawText);
    const auto normalizedCandidate = LearningKey(candidate);
    const auto position = std::lower_bound(
        preferences_.begin(), preferences_.end(),
        std::pair{std::wstring_view(normalizedRaw), std::wstring_view(normalizedCandidate)},
        [](const auto& existing, const auto& value) {
            return std::pair<std::wstring_view, std::wstring_view>{existing.rawText,
                                                                    existing.candidate} < value;
        });
    return position != preferences_.end() && position->rawText == normalizedRaw &&
                   position->candidate == normalizedCandidate
               ? &*position
               : nullptr;
}

void UserLearningStore::RecordExposure(std::wstring_view rawText,
                                       std::wstring_view candidate) {
    if (!rawText.empty() && !candidate.empty()) {
        AddCount(FindOrAddPreference(rawText, candidate).exposure);
    }
}

void UserLearningStore::RecordSelection(std::wstring_view rawText,
                                        std::wstring_view candidate) {
    if (rawText.empty() || candidate.empty()) return;
    auto& preference = FindOrAddPreference(rawText, candidate);
    AddMass(preference.alpha, 1.0);
    AddMass(preference.directEventMass, 1.0);
}

void UserLearningStore::RecordAutomaticAcceptance(std::wstring_view rawText,
                                                   std::wstring_view candidate) {
    if (rawText.empty() || candidate.empty()) return;
    auto& preference = FindOrAddPreference(rawText, candidate);
    // An implicit boundary acceptance is weaker than an explicit choice.
    AddMass(preference.alpha, 0.25);
    AddMass(preference.directEventMass, 0.25);
}

void UserLearningStore::RecordNonSelection(std::wstring_view rawText,
                                           std::wstring_view candidate) {
    if (rawText.empty() || candidate.empty()) return;
    auto& preference = FindOrAddPreference(rawText, candidate);
    AddMass(preference.beta, 0.12);
    AddMass(preference.directEventMass, 0.12);
}

void UserLearningStore::RecordUndo(std::wstring_view rawText,
                                   std::wstring_view candidate) {
    if (rawText.empty() || candidate.empty()) return;
    auto& preference = FindOrAddPreference(rawText, candidate);
    AddMass(preference.beta, kUndoMass);
    AddMass(preference.directEventMass, kUndoMass);
}

double UserLearningStore::Score(std::wstring_view rawText,
                                std::wstring_view candidate) const noexcept {
    const auto* preference = FindPreference(rawText, candidate);
    if (!preference || preference->directEventMass < 1.0) return 0.0;
    const auto total = std::max(2.0, preference->alpha + preference->beta);
    const auto mean = preference->alpha / total;
    const auto confidence = 1.0 - std::exp(-(total - 2.0) / 3.0);
    return std::clamp(confidence * (mean - 0.5), -0.5, 0.5);
}

bool UserLearningStore::Undone(std::wstring_view rawText,
                               std::wstring_view candidate) const noexcept {
    // One undo adds kUndoMass to beta; taking the correction adds to alpha.
    const auto* preference = FindPreference(rawText, candidate);
    return preference && preference->beta >= kUndoMass && preference->beta > preference->alpha;
}

void UserLearningStore::Reset() noexcept {
    entries_.clear();
    preferences_.clear();
}

std::span<const UserLearningEntry> UserLearningStore::Entries() const noexcept {
    return entries_;
}

std::span<const UserLearningPreference> UserLearningStore::Preferences() const noexcept {
    return preferences_;
}

bool SocialLearningStore::Add(SocialLearningEntry entry) {
    if (entry.trigger.empty() || entry.candidate.empty()) return false;
    const auto position = std::lower_bound(
        entries_.begin(), entries_.end(), entry,
        [](const auto& existing, const auto& value) {
            return std::pair{existing.trigger, existing.candidate} <
                   std::pair{value.trigger, value.candidate};
        });
    if (position != entries_.end() && position->trigger == entry.trigger &&
        position->candidate == entry.candidate) return false;
    entries_.insert(position, std::move(entry));
    return true;
}

SocialLearningEntry& SocialLearningStore::FindOrAdd(std::wstring_view trigger,
                                                     std::wstring_view candidate) {
    const auto position = std::lower_bound(
        entries_.begin(), entries_.end(), std::pair{trigger, candidate},
        [](const auto& existing, const auto& value) {
            return std::pair<std::wstring_view, std::wstring_view>{existing.trigger,
                                                                    existing.candidate} < value;
        });
    if (position != entries_.end() && position->trigger == trigger &&
        position->candidate == candidate) return *position;
    return *entries_.insert(position, {std::wstring(trigger), std::wstring(candidate)});
}

const SocialLearningEntry* SocialLearningStore::Find(std::wstring_view trigger,
                                                     std::wstring_view candidate) const noexcept {
    const auto position = std::lower_bound(
        entries_.begin(), entries_.end(), std::pair{trigger, candidate},
        [](const auto& existing, const auto& value) {
            return std::pair<std::wstring_view, std::wstring_view>{existing.trigger,
                                                                    existing.candidate} < value;
        });
    return position != entries_.end() && position->trigger == trigger &&
                   position->candidate == candidate
               ? &*position
               : nullptr;
}

void SocialLearningStore::RecordExposure(std::wstring_view trigger,
                                         std::wstring_view candidate) {
    if (!trigger.empty() && !candidate.empty()) ++FindOrAdd(trigger, candidate).exposure;
}

void SocialLearningStore::RecordSelection(std::wstring_view trigger,
                                          std::wstring_view candidate) {
    if (trigger.empty() || candidate.empty()) return;
    auto& entry = FindOrAdd(trigger, candidate);
    entry.alpha += 1.0;
    entry.directEventMass += 1.0;
}

void SocialLearningStore::RecordNonSelection(std::wstring_view trigger,
                                             std::wstring_view candidate) {
    if (!trigger.empty() && !candidate.empty()) FindOrAdd(trigger, candidate).beta += 0.12;
}

void SocialLearningStore::RecordUndo(std::wstring_view trigger,
                                     std::wstring_view candidate) {
    if (trigger.empty() || candidate.empty()) return;
    auto& entry = FindOrAdd(trigger, candidate);
    entry.beta += 1.5;
    entry.directEventMass += 1.5;
}

double SocialLearningStore::Score(std::wstring_view trigger,
                                  std::wstring_view candidate) const noexcept {
    const auto* entry = Find(trigger, candidate);
    if (!entry || entry->directEventMass < 2.0) return 0.0;
    const auto totalExposure = std::max<std::uint64_t>(1, entry->exposure);
    const auto total = std::max(2.0, entry->alpha + entry->beta);
    const auto mean = entry->alpha / total;
    const auto confidence = 1.0 - std::exp(-(total - 2.0) / 3.0);
    const auto exploration = 0.08 * std::sqrt(std::log(static_cast<double>(totalExposure) + 2.0) /
                                                (static_cast<double>(entry->exposure) + 1.0));
    return confidence * (mean - 0.5) + exploration;
}

void SocialLearningStore::Reset() noexcept { entries_.clear(); }

std::span<const SocialLearningEntry> SocialLearningStore::Entries() const noexcept {
    return entries_;
}

double SocialLearningProvider::Score(std::wstring_view trigger,
                                     std::wstring_view candidate) const noexcept {
    if (level_ <= 0) return 0.0;
    const double weight = level_ == 1 ? 0.20 : level_ == 2 ? 0.42 : level_ == 3 ? 0.70 : 1.0;
    return store_.Score(trigger, candidate) * weight;
}

}  // namespace tekito
