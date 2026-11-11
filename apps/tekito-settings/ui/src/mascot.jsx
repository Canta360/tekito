// Home's companion: TEKITO's outline mark (assets/icons/symbols/
// tekito-outline.svg) in a thinner line, without the T, with eyes. It answers
// with its eyes and how it moves:
//   idle       floats and blinks, eyes open; its eyes on the pointer (`follow`)
//   listening  the user is typing: eyes wider, on the box
//   searching  AI is looking: half-shut eyes going side to side as it sways
//   happy      a setting was changed: it hops, eyes ∩ ∩
//   unsure     it could not tell which setting: it tilts, eyes · .
//   down       AI did not answer: it sags, eyes spinning @ @
// Also the icon beside AI's replies. Still for those who ask Windows for
// less motion.
import React, { useEffect, useRef, useState } from "react";

// The mark's circle (center 450, 450; radius 340) and its small ring.
const kRing = "M582 763A340 340 0 1 1 770 565";
const kDot = { cx: 709, cy: 695, r: 82 };
const kEyes = [360, 540];
const kEyeY = 440;

function eye(mood, cx) {
  const y = kEyeY;
  switch (mood) {
    case "happy":
      return <path d={`M${cx - 48} ${y + 18}A48 48 0 0 1 ${cx + 48} ${y + 18}`} />;
    case "searching":
      return <path d={`M${cx - 52} ${y - 14}H${cx + 52}A52 52 0 0 1 ${cx - 52} ${y - 14}Z`} />;
    case "unsure":
      // Not level: the right one lower.
      return <circle cx={cx} cy={cx > 450 ? y + 30 : y - 12} r="20" className="mascot-dot" />;
    case "down": {
      // A spiral, out from the middle, turning.
      const r = [12, 24, 36, 48, 60];
      let d = `M${cx + 12} ${y}`;
      r.forEach((radius, i) => {
        d += i % 2 === 0 ? `A${radius} ${radius} 0 0 0 ${cx - radius} ${y}` : `A${radius} ${radius} 0 0 0 ${cx + radius + 12} ${y}`;
      });
      return <path className="mascot-spiral" style={{ transformOrigin: `${cx + 6}px ${y}px` }} d={d} />;
    }
    default:
      return <circle className="mascot-open" cx={cx} cy={y} r={mood === "listening" ? 54 : 46} style={{ transformOrigin: `${cx}px ${y}px` }} />;
  }
}

export function Mascot({ mood = "idle", size = 96, follow = false, label }) {
  const ref = useRef(null);
  const [look, setLook] = useState({ x: 0, y: 0 });
  useEffect(() => {
    if (!follow || mood !== "idle") {
      setLook({ x: 0, y: mood === "listening" ? 30 : 0 });
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
        // Up to 50 of the drawing's units toward the pointer.
        const reach = Math.min(1, distance / 180) * 50;
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
      <g className="mascot-body">
        <path className="mascot-ring" d={kRing} />
        <circle className="mascot-small" cx={kDot.cx} cy={kDot.cy} r={kDot.r} />
        <g className="mascot-face" style={{ transform: `translate(${look.x}px, ${look.y}px)` }}>
          {kEyes.map((cx) => <g key={cx}>{eye(mood, cx)}</g>)}
        </g>
      </g>
    </svg>
  );
}
