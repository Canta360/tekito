// Written by apps/tekito-settings/ui/write-companion-shapes.mjs from
// apps/tekito-settings/ui/src/companionShapes.js; change that, then run it.
#pragma once

namespace tekito::companion {

struct Stop {
    float offset;
    unsigned rgb;
};

struct EyeShape {
    const char* path;
    // A line drawn with a round pen, not a filled shape.
    bool line;
};

// Drawn in the box (60, 60) to (860, 860).
inline constexpr float kViewLeft = 60.0f;
inline constexpr float kViewSize = 800.0f;

inline constexpr const char* kDisc = "M88 450A362 362 0 0 1 812 450C812 640 680 768 450 768C220 768 88 640 88 450Z";
inline constexpr float kCutX = 709.0f;
inline constexpr float kCutY = 695.0f;
inline constexpr float kCutRadius = 120.0f;
inline constexpr float kRingX = 709.0f;
inline constexpr float kRingY = 695.0f;
inline constexpr float kRingOuter = 86.0f;
inline constexpr float kRingHole = 35.0f;
inline constexpr Stop kBodyStops[] = {{0.0f, 0xa6c8ff}, {0.3f, 0x3f88ff}, {0.55f, 0x0668ff}, {0.75f, 0x0590ff}, {1.0f, 0x00dcff}};
inline constexpr Stop kRingStops[] = {{0.0f, 0x6fe8ff}, {1.0f, 0x10b6fe}};
inline constexpr float kSheenX = 0.18f;
inline constexpr float kSheenY = 0.14f;
inline constexpr float kSheenRadius = 0.62f;
inline constexpr float kSheenOpacity = 0.6f;
inline constexpr float kBlueHue = 216.0f;

inline constexpr float kEyeLeft = 328.0f;
inline constexpr float kEyeRight = 632.0f;
inline constexpr float kEyeY = 404.0f;
inline constexpr float kEyeScale = 1.15f;
inline constexpr EyeShape kEyeOpen{"M-100 0C-100 -52 -62 -84 0 -84C62 -84 100 -52 100 0C100 52 62 84 0 84C-62 84 -100 52 -100 0Z", false};
inline constexpr EyeShape kEyeHappy{"M-100 34C-100 -36 -58 -76 0 -76C58 -76 100 -36 100 34Q100 44 90 44H-90Q-100 44 -100 34Z", false};
inline constexpr EyeShape kEyeHalf{"M-100 -34Q-100 -44 -90 -44H90Q100 -44 100 -34C100 34 58 64 0 64C-58 64 -100 34 -100 -34Z", false};
inline constexpr EyeShape kEyeSad{"M-100 -12Q-100 -22 -90 -24L88 -52Q100 -54 100 -42C100 30 58 64 0 64C-58 64 -100 38 -100 -12Z", false};
inline constexpr EyeShape kEyeShut{"M-96 -24Q-10 -40 92 12", true};
inline constexpr const char* kTear = "M-46 46C-44 70 -50 84 -50 96A20 20 0 0 0 -10 96C-10 84 -16 70 -14 46Z";

}  // namespace tekito::companion
