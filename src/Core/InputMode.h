#pragma once

namespace tekito {

// Direct types the keys as pressed; Convert is English Auto. Japanese
// exists only in the ja-JP profile, which switches among all three.
enum class InputMode {
    Direct,
    Convert,
    Japanese,
};

}  // namespace tekito
