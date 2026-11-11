// Home's companion: TEKITO's filled mark (assets/icons/symbols/
// tekito-filled.svg) without the T, with white eyes. It answers with its eyes
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
import { eyeShapes, tear } from "./companionEyes.js";

// The mark: a disc (center 450, 450; radius 362) with room cut around its
// small ring (center 709, 695), the ring a little smaller than the mark's
// (86 across the outside, a 35 hole; the mark's 108 and 44).
const kSmall = "M709 609a86 86 0 1 0 0.01 0ZM709 660a35 35 0 1 1 -0.01 0Z";
// Large white eyes (companionEyes.js), as wide apart as the user drew them.
const kEyes = [298, 602];
const kEyeY = 430;
// companionEyes.js's shapes made larger.
const kEyeScale = 1.15;

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

export function Mascot({ mood = "idle", size = 96, follow = false, label }) {
  const ref = useRef(null);
  const cut = `mascot-cut-${useId().replace(/:/g, "")}`;
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
        <mask id={cut} maskUnits="userSpaceOnUse" x="0" y="0" width="960" height="960">
          <rect width="960" height="960" fill="#fff" />
          <circle cx="709" cy="695" r="120" fill="#000" />
        </mask>
      </defs>
      <g className="mascot-body">
        <circle className="mascot-disc" cx="450" cy="450" r="362" mask={`url(#${cut})`} />
        <path className="mascot-small" d={kSmall} fillRule="evenodd" />
        <g className="mascot-face" style={{ transform: `translate(${look.x}px, ${look.y}px)` }}>
          {eyes(mood)}
        </g>
      </g>
    </svg>
  );
}
