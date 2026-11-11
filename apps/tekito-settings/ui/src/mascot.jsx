// Home's companion: TEKITO's filled mark (assets/icons/symbols/
// tekito-filled.svg) without the T, with white eyes. It answers with its eyes
// and how it moves:
//   idle       floats and blinks, eyes open; its eyes on the pointer (`follow`)
//   listening  the user is typing: eyes wider, on the box
//   searching  AI is looking: half-shut eyes going side to side as it sways
//   happy      a setting was changed: it hops, eyes ∩ ∩
//   unsure     it could not tell which setting: it tilts, one eye smaller
//   down       AI did not answer: it sags, eyes half shut and drooping, a drop
// Its moves are small.
// Also the icon beside AI's replies. Still for those who ask Windows for
// less motion.
import React, { useEffect, useId, useRef, useState } from "react";

// The mark: a disc (center 450, 450; radius 362) with room cut around its
// small ring (center 709, 695; 108 across the outside, a 44 hole).
const kSmall = "M709 587a108 108 0 1 0 0.01 0ZM709 651a44 44 0 1 1 -0.01 0Z";
// Large white eyes, as wide apart as the user drew them.
const kEyes = [298, 602];
const kEyeY = 430;
const kRx = 104;
const kRy = 80;

function eye(mood, cx) {
  const y = kEyeY;
  const left = cx < 450;
  switch (mood) {
    case "happy":
      return <path d={`M${cx - 86} ${y + 30}A86 74 0 0 1 ${cx + 86} ${y + 30}`} />;
    case "searching":
      // Half shut: a level lid over the lower half.
      return <path className="mascot-white" d={`M${cx - kRx} ${y - 6}H${cx + kRx}A${kRx} ${kRy} 0 0 1 ${cx - kRx} ${y - 6}Z`} />;
    case "down": {
      // Half shut and drooping outward, with a drop under the right eye.
      const lid = left ? [y + 6, y - 22] : [y - 22, y + 6];
      return (
        <>
          <path className="mascot-white" d={`M${cx - kRx} ${lid[0]}L${cx + kRx} ${lid[1]}A${kRx} ${kRy} 0 0 1 ${cx - kRx} ${lid[0]}Z`} />
          {!left && <path className="mascot-white mascot-drop" d={`M${cx + 48} ${y + 62}C${cx + 50} ${y + 96} ${cx + 30} ${y + 100} ${cx + 30} ${y + 118}A20 20 0 0 0 ${cx + 70} ${y + 118}C${cx + 70} ${y + 100} ${cx + 52} ${y + 96} ${cx + 54} ${y + 58}`} />}
        </>
      );
    }
    case "unsure":
      // One eye smaller and lower.
      return <ellipse className="mascot-white" cx={cx} cy={left ? y : y + 18} rx={left ? kRx : kRx * 0.72} ry={left ? kRy : kRy * 0.72} />;
    default:
      return <ellipse className="mascot-white mascot-open" cx={cx} cy={y} rx={kRx} ry={mood === "listening" ? kRy + 8 : kRy} style={{ transformOrigin: `${cx}px ${y}px` }} />;
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
          <circle cx="709" cy="695" r="150" fill="#000" />
        </mask>
      </defs>
      <g className="mascot-body">
        <circle className="mascot-disc" cx="450" cy="450" r="362" mask={`url(#${cut})`} />
        <path className="mascot-small" d={kSmall} fillRule="evenodd" />
        <g className="mascot-face" style={{ transform: `translate(${look.x}px, ${look.y}px)` }}>
          {kEyes.map((cx) => <g key={cx}>{eye(mood, cx)}</g>)}
        </g>
      </g>
    </svg>
  );
}
