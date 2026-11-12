// Home's companion (its shapes and colors: companionShapes.js), with white
// eyes. It answers with its eyes
// and how it moves:
//   idle       floats and blinks, eyes open; its eyes on the pointer (`follow`)
//   listening  the user is typing: eyes wider, on the box
//   searching  AI is looking: half-shut eyes going side to side as it sways
//   happy      a setting was changed: it hops, eyes the upper halves
//   unsure     it could not tell which setting: it tilts, one eye smaller
//   down       AI did not answer: it sags, eyes half shut and drooping, a drop
// Its moves are small.
// Also the icon beside AI's replies. Still for those who ask Windows for
// less motion.
import React, { useEffect, useId, useRef, useState } from "react";
import { body, eyeShapes, face, tear } from "./companionShapes.js";

// The body, the eyes and the colors: companionShapes.js.
const kDisc = body.disc;
const kSmall = (({ cx, cy, outer, hole }) =>
  `M${cx} ${cy - outer}a${outer} ${outer} 0 1 0 0.01 0ZM${cx} ${cy - hole}a${hole} ${hole} 0 1 1 -0.01 0Z`)(body.ring);
const kEyes = face.eyes;
const kEyeY = face.y;
const kEyeScale = face.scale;

// One shape for both eyes; the right one mirrored. `scale` and `drop` per
// eye: [left, right].
function pair(name, { scale = [1, 1], drop = [0, 0], className = "" } = {}) {
  const shape = eyeShapes[name];
  return kEyes.map((cx, i) => (
    <path key={`${name}${cx}`} className={shape.line ? `mascot-line ${className}` : `mascot-white ${className}`} d={shape.fill || shape.line}
      transform={`translate(${cx} ${kEyeY + drop[i]}) scale(${(i ? -1 : 1) * scale[i] * kEyeScale} ${scale[i] * kEyeScale})`} />
  ));
}

function eyes(mood) {
  switch (mood) {
    case "happy":
      return pair("happy");
    case "searching":
      return pair("half");
    case "down":
      return [...pair("sad"), <path key="tear" className="mascot-white" d={tear} transform={`translate(${kEyes[1]} ${kEyeY}) scale(${kEyeScale})`} />];
    case "unsure":
      // The right one smaller and lower.
      return pair("open", { scale: [1, 0.72], drop: [0, 18] });
    default:
      // Open, shut for a moment now and then.
      return [...pair("open", { scale: mood === "listening" ? [1.08, 1.08] : [1, 1], className: "mascot-open" }),
        ...pair("shut", { className: "mascot-shut" })];
  }
}

const kBodyStops = body.stops;
const kRingStops = body.ringStops;
const kBlueHue = body.blueHue;

function toHsl(hex) {
  const [r, g, b] = [1, 3, 5].map((at) => parseInt(hex.slice(at, at + 2), 16) / 255);
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const l = (max + min) / 2;
  const d = max - min;
  if (d === 0) return [0, 0, l];
  const s = d / (1 - Math.abs(2 * l - 1));
  const h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h * 60, s, l];
}

// Rainbow: the companion's colors with their hues turned so its blue is
// `hue` (the accent color's), as CSS variables for main.jsx to set.
export function mascotRainbow(hue) {
  const turned = (color) => {
    const [h, s, l] = toHsl(color);
    return `hsl(${Math.round((((h + hue - kBlueHue) % 360) + 360) % 360)} ${Math.round(s * 100)}% ${Math.round(l * 100)}%)`;
  };
  return [...kBodyStops.map(([, color], i) => [`--mascot-${i}`, turned(color)]),
    ...kRingStops.map(([, color], i) => [`--mascot-ring-${i}`, turned(color)])];
}

export function Mascot({ mood = "idle", size = 96, follow = false, label }) {
  const ref = useRef(null);
  const id = useId().replace(/:/g, "");
  const cut = `mascot-cut-${id}`;
  const [look, setLook] = useState({ x: 0, y: 0 });
  useEffect(() => {
    if (!follow || mood !== "idle") {
      setLook({ x: 0, y: mood === "listening" ? 12 : 0 });
      return undefined;
    }
    let frame = 0;
    const onMove = (event) => {
      if (frame) return;
      frame = window.requestAnimationFrame(() => {
        frame = 0;
        const box = ref.current?.getBoundingClientRect();
        if (!box) return;
        const dx = event.clientX - (box.left + box.width / 2);
        const dy = event.clientY - (box.top + box.height / 2);
        const distance = Math.hypot(dx, dy) || 1;
        // Up to 20 of the drawing's units toward the pointer: a glance.
        const reach = Math.min(1, distance / 240) * 20;
        setLook({ x: (dx / distance) * reach, y: (dy / distance) * reach * 0.8 });
      });
    };
    window.addEventListener("pointermove", onMove);
    return () => {
      window.removeEventListener("pointermove", onMove);
      if (frame) window.cancelAnimationFrame(frame);
    };
  }, [follow, mood]);

  return (
    <svg ref={ref} className={`mascot is-${mood}`} width={size} height={size} viewBox="60 60 800 800"
      role={label ? "img" : undefined} aria-label={label || undefined} aria-hidden={label ? undefined : true}>
      <defs>
        <linearGradient id={`mascot-body-${id}`} x1="0.1" y1="0.05" x2="0.9" y2="0.95">
          {kBodyStops.map(([offset, color], i) => <stop key={offset} offset={offset} style={{ stopColor: `var(--mascot-${i}, ${color})` }} />)}
        </linearGradient>
        <linearGradient id={`mascot-ring-${id}`} x1="0" y1="0" x2="1" y2="1">
          {kRingStops.map(([offset, color], i) => <stop key={offset} offset={offset} style={{ stopColor: `var(--mascot-ring-${i}, ${color})` }} />)}
        </linearGradient>
        <radialGradient id={`mascot-sheen-${id}`} cx={body.sheen.cx} cy={body.sheen.cy} r={body.sheen.r}>
          <stop offset="0" stopColor="#ffffff" stopOpacity={body.sheen.opacity} />
          <stop offset="1" stopColor="#ffffff" stopOpacity="0" />
        </radialGradient>
        <mask id={cut} maskUnits="userSpaceOnUse" x="0" y="0" width="960" height="960">
          <rect width="960" height="960" fill="#fff" />
          <circle cx={body.cut.cx} cy={body.cut.cy} r={body.cut.r} fill="#000" />
        </mask>
      </defs>
      <g className="mascot-body">
        <path className="mascot-disc" d={kDisc} mask={`url(#${cut})`} fill={`url(#mascot-body-${id})`} />
        <path d={kDisc} mask={`url(#${cut})`} fill={`url(#mascot-sheen-${id})`} />
        <path className="mascot-small" d={kSmall} fillRule="evenodd" fill={`url(#mascot-ring-${id})`} />
        <g className="mascot-face" style={{ transform: `translate(${look.x}px, ${look.y}px)` }}>
          {eyes(mood)}
        </g>
      </g>
    </svg>
  );
}
