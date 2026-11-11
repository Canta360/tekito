// Fluent 2's theme in the Windows accent color: a brand ramp of 16 shades
// around it (Fluent's BrandVariants, 10 to 160, the accent at 80), light or
// dark as Windows is, in the TEKITO typeface.
import { createDarkTheme, createLightTheme } from "@fluentui/react-components";

const font = '"M PLUS 1", "Segoe UI Variable Text", "Segoe UI", "Yu Gothic UI", system-ui, sans-serif';

const parse = (hex) => {
  const value = hex.replace("#", "");
  const full = value.length === 3 ? value.split("").map((c) => c + c).join("") : value;
  return [0, 2, 4].map((at) => parseInt(full.slice(at, at + 2), 16) || 0);
};
const format = (rgb) => `#${rgb.map((c) => Math.round(Math.min(255, Math.max(0, c))).toString(16).padStart(2, "0")).join("")}`;
const mix = (from, to, amount) => from.map((c, i) => c + (to[i] - c) * amount);

export function brandRamp(accent) {
  const base = parse(accent);
  const ramp = {};
  for (let shade = 10; shade <= 160; shade += 10) {
    ramp[shade] = format(shade <= 80
      ? mix([0, 0, 0], base, 0.18 + 0.82 * ((shade - 10) / 70))
      : mix(base, [255, 255, 255], 0.88 * ((shade - 80) / 80)));
  }
  return ramp;
}

export function tekitoTheme(accent, dark) {
  const brand = brandRamp(accent || "#0067c0");
  return { ...(dark ? createDarkTheme(brand) : createLightTheme(brand)), fontFamilyBase: font };
}

// The theme's brand colors, as the CSS variables Fluent reads them from.
export function brandVariables(accent, dark) {
  return Object.entries(tekitoTheme(accent, dark)).filter(([name]) => name.includes("Brand")).map(([name, value]) => [`--${name}`, value]);
}

// A hue, as #rrggbb (the Rainbow option's accent).
export function hueColor(hue) {
  const s = 0.85;
  const l = 0.45;
  const k = (n) => (n + hue / 30) % 12;
  const a = s * Math.min(l, 1 - l);
  const f = (n) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)));
  return format([f(0) * 255, f(8) * 255, f(4) * 255]);
}
