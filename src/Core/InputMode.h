#pragma once

namespace tekito {

// Direct types the keys as pressed; Convert is English Auto. Japanese
// exists only in the ja-JP profile, which switches among all of them.
// Mixed is Japanese typed with English words in it, a chunk before each
// Space (JapaneseComposer::SetMixedTyping), when Settings turns it on.
enum class InputMode {
    Direct,
    Convert,
    Japanese,
    Mixed,
};

// Japanese and Mixed both type with the Japanese composer.
[[nodiscard]] constexpr bool TypesJapanese(InputMode mode) noexcept {
    return mode == InputMode::Japanese || mode == InputMode::Mixed;
}

}  // namespace tekito
