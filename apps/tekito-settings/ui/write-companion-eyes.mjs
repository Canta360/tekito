// Writes the companion's eyes (src/companionEyes.js) as SVG files in
// assets/icons/companion/, both eyes in each, drawn in a line as the user
// drew them: node write-companion-eyes.mjs
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { eyeShapes, tearLine } from "./src/companionEyes.js";

const here = dirname(fileURLToPath(import.meta.url));
const target = join(here, "..", "..", "..", "assets", "icons", "companion");
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
console.log(`wrote ${Object.keys(eyeShapes).length} files to ${target}`);
