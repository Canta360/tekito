#include "Core/ConversionEngine.h"

#include "Core/CandidateEngine.h"
#include "Core/UserDictionary.h"

#include <cwctype>

namespace tekito {

bool IsSentenceStart(std::wstring_view precedingText) noexcept {
    auto end = precedingText.size();
    while (end > 0) {
        const auto ch = precedingText[end - 1];
        if (ch == L' ' || ch == L'\t' || ch == L'\"' || ch == L'\'' ||
            ch == L')' || ch == L']' || ch == L'}') {
            --end;
            continue;
        }
        return ch == L'\r' || ch == L'\n' || ch == L'.' || ch == L'!' || ch == L'?';
    }
    return true;
}

bool NeedsTerminalPeriod(std::wstring_view lineText) noexcept {
    const auto lineStart = lineText.find_last_of(L"\r\n");
    auto start = lineStart == std::wstring_view::npos ? 0 : lineStart + 1;
    auto end = lineText.size();
    while (start < end && (lineText[start] == L' ' || lineText[start] == L'\t')) ++start;
    while (end > start && (lineText[end - 1] == L' ' || lineText[end - 1] == L'\t')) --end;
    if (start == end) return false;
    const auto last = lineText[end - 1];
    return last != L'.' && last != L'!' && last != L'?' && last != L',' &&
           last != L':' && last != L';' && last != L'\"' && last != L'\'' &&
           last != L')' && last != L']' && last != L'}';
}

namespace {

template <typename... Args>
std::unique_ptr<IConversionEngine> CreateEngine(const Args&... args) noexcept {
    try {
        return std::make_unique<CandidateEngine>(args...);
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine() noexcept {
    return CreateEngine();
}

std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine(
    const UserDictionary& dictionary) noexcept {
    return CreateEngine(dictionary);
}

std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine(
    const UserDictionary& dictionary,
    const IUserLearningProvider& learningProvider) noexcept {
    return CreateEngine(dictionary, learningProvider);
}

std::unique_ptr<IConversionEngine> CreateDefaultConversionEngine(
    const UserDictionary& dictionary,
    const IUserLearningProvider& learningProvider,
    const SocialLearningProvider& socialLearningProvider) noexcept {
    return CreateEngine(dictionary, learningProvider, socialLearningProvider);
}

}  // namespace tekito
