// Writes the companion's shapes (src/companionShapes.js) for the desktop
// companion, as apps/tekito-settings/CompanionShapes.h, and its eyes as SVG
// files in assets/icons/companion/, both eyes in each, drawn in a line as
// the user drew them: node write-companion-shapes.mjs
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { body, eyeShapes, face, tear, tearLine } from "./src/companionShapes.js";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..", "..");

// The eyes as files.
const target = join(root, "assets", "icons", "companion");
mkdirSync(target, { recursive: true });
// The two eyes 300 apart, as on the companion.
const eyes = [[150, "translate(150 130)"], [450, "translate(450 130) scale(-1 1)"]];
for (const [name, shape] of Object.entries(eyeShapes)) {
  const d = shape.fill || shape.line;
  const parts = eyes.map(([, at]) => `<path transform="${at}" d="${d}"/>`);
  if (name === "sad") parts.push(`<path transform="translate(450 130)" d="${tearLine}"/>`);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 600 260"><g fill="none" stroke="currentColor" stroke-width="12" stroke-linecap="round" stroke-linejoin="round">${parts.join("")}</g></svg>\n`;
  writeFileSync(join(target, `eyes-${name}.svg`), svg);
}

// The header.
const number = (value) => (Number.isInteger(value) ? `${value}.0f` : `${value}f`);
const stops = (list) => list.map(([offset, color]) => `{${number(offset)}, 0x${color.slice(1)}}`).join(", ");
const eyeName = (name) => `kEye${name[0].toUpperCase()}${name.slice(1)}`;
const header = `// Written by apps/tekito-settings/ui/write-companion-shapes.mjs from
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

inline constexpr const char* kDisc = "${body.disc}";
inline constexpr float kCutX = ${number(body.cut.cx)};
inline constexpr float kCutY = ${number(body.cut.cy)};
inline constexpr float kCutRadius = ${number(body.cut.r)};
inline constexpr float kRingX = ${number(body.ring.cx)};
inline constexpr float kRingY = ${number(body.ring.cy)};
inline constexpr float kRingOuter = ${number(body.ring.outer)};
inline constexpr float kRingHole = ${number(body.ring.hole)};
inline constexpr Stop kBodyStops[] = {${stops(body.stops)}};
inline constexpr Stop kRingStops[] = {${stops(body.ringStops)}};
inline constexpr float kSheenX = ${number(body.sheen.cx)};
inline constexpr float kSheenY = ${number(body.sheen.cy)};
inline constexpr float kSheenRadius = ${number(body.sheen.r)};
inline constexpr float kSheenOpacity = ${number(body.sheen.opacity)};
inline constexpr float kBlueHue = ${number(body.blueHue)};

inline constexpr float kEyeLeft = ${number(face.eyes[0])};
inline constexpr float kEyeRight = ${number(face.eyes[1])};
inline constexpr float kEyeY = ${number(face.y)};
inline constexpr float kEyeScale = ${number(face.scale)};
${Object.entries(eyeShapes).map(([name, shape]) => `inline constexpr EyeShape ${eyeName(name)}{"${shape.fill || shape.line}", ${shape.line ? "true" : "false"}};`).join("\n")}
inline constexpr const char* kTear = "${tear}";

}  // namespace tekito::companion
`;
writeFileSync(join(root, "apps", "tekito-settings", "CompanionShapes.h"), header);
console.log("wrote assets/icons/companion and apps/tekito-settings/CompanionShapes.h");
