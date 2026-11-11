// Home's companion: a ball in the Windows accent color, shaded to look
// round, with eyes. It answers with its face and how it moves:
//   idle       floats and blinks; its eyes on the pointer (`follow`)
//   listening  the user is typing: it looks at the box
//   searching  AI is looking: its eyes go side to side as it sways
//   happy      a setting was changed: it hops, eyes ^ ^
//   unsure     it could not tell which setting: it tilts, one eye bigger
//   down       AI did not answer: it sags, eyes half shut
// Also the icon beside AI's replies. Still for those who ask Windows for
// less motion.
import React, { useEffect, useId, useRef, useState } from "react";

export function Mascot({ mood = "idle", size = 96, follow = false, label }) {
  const ref = useRef(null);
  const id = useId().replace(/:/g, "");
  const [look, setLook] = useState({ x: 0, y: 0 });
  useEffect(() => {
    if (!follow || mood !== "idle") {
      setLook({ x: 0, y: mood === "listening" ? 3 : 0 });
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
        // Up to 5 of the drawing's units toward the pointer.
        const reach = Math.min(1, distance / 180) * 5;
        setLook({ x: (dx / distance) * reach, y: (dy / distance) * reach * 0.8 });
      });
    };
    window.addEventListener("pointermove", onMove);
    return () => {
      window.removeEventListener("pointermove", onMove);
      if (frame) window.cancelAnimationFrame(frame);
    };
  }, [follow, mood]);

  const eye = (cx) => {
    if (mood === "happy") return <path key={cx} d={`M${cx - 5} 50 Q${cx} 42 ${cx + 5} 50`} fill="none" stroke="var(--mascot-eye)" strokeWidth="3.4" strokeLinecap="round" />;
    if (mood === "down") return <path key={cx} d={`M${cx - 5} 48 Q${cx} 53 ${cx + 5} 48`} fill="none" stroke="var(--mascot-eye)" strokeWidth="3.2" strokeLinecap="round" />;
    const big = mood === "unsure" && cx > 48 ? 1.25 : mood === "listening" ? 1.1 : 1;
    return (
      <g key={cx} className="mascot-eye" style={{ transformOrigin: `${cx}px 48px` }}>
        <ellipse cx={cx} cy="48" rx={4.2 * big} ry={6 * big} fill="var(--mascot-eye)" />
        <circle cx={cx + 1.4} cy={45.6} r={1.5 * big} fill="#fff" opacity="0.9" />
      </g>
    );
  };

  return (
    <svg ref={ref} className={`mascot is-${mood}`} width={size} height={size} viewBox="0 0 96 96"
      role={label ? "img" : undefined} aria-label={label || undefined} aria-hidden={label ? undefined : true}>
      <defs>
        <radialGradient id={`ball-${id}`} cx="0.36" cy="0.3" r="0.78">
          <stop offset="0" style={{ stopColor: "var(--mascot-light)" }} />
          <stop offset="0.55" style={{ stopColor: "var(--mascot-mid)" }} />
          <stop offset="1" style={{ stopColor: "var(--mascot-dark)" }} />
        </radialGradient>
        <radialGradient id={`shine-${id}`} cx="0.5" cy="0.5" r="0.5">
          <stop offset="0" stopColor="#fff" stopOpacity="0.85" />
          <stop offset="1" stopColor="#fff" stopOpacity="0" />
        </radialGradient>
      </defs>
      <ellipse className="mascot-shadow" cx="48" cy="88" rx="22" ry="4" fill="var(--mascot-shadow)" />
      <g className="mascot-ball">
        <circle cx="48" cy="48" r="32" fill={`url(#ball-${id})`} />
        <ellipse cx="36" cy="30" rx="12" ry="7" fill={`url(#shine-${id})`} transform="rotate(-25 36 30)" />
        <g className="mascot-face" style={{ transform: `translate(${look.x}px, ${look.y}px)` }}>
          {eye(39)}
          {eye(57)}
          <ellipse cx="31" cy="58" rx="5" ry="2.6" fill="var(--mascot-cheek)" />
          <ellipse cx="65" cy="58" rx="5" ry="2.6" fill="var(--mascot-cheek)" />
        </g>
      </g>
    </svg>
  );
}
