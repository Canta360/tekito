// Home's companion: a round cat (the user's sketch), in Settings' single
// line. Moods: "idle" (・ω・, its eyes on the pointer when `follow`),
// "searching" (−ω−, a magnifying glass going side to side), "happy"
// (content, after a change). Also the icon beside AI's replies. Still when
// the user asks Windows for less motion.
import React, { useEffect, useRef, useState } from "react";

export function Mascot({ mood = "idle", size = 88, follow = false, label }) {
  const ref = useRef(null);
  const [look, setLook] = useState({ x: 0, y: 0 });
  useEffect(() => {
    if (!follow || mood !== "idle") return undefined;
    let frame = 0;
    const onMove = (event) => {
      if (frame) return;
      frame = window.requestAnimationFrame(() => {
        frame = 0;
        const box = ref.current?.getBoundingClientRect();
        if (!box) return;
        const dx = event.clientX - (box.left + box.width / 2);
        const dy = event.clientY - (box.top + box.height * 0.58);
        const distance = Math.hypot(dx, dy) || 1;
        // Up to 3.5 of the drawing's units toward the pointer.
        const reach = Math.min(1, distance / 160) * 3.5;
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
    <svg ref={ref} className={`mascot is-${mood}`} width={size} height={size} viewBox="0 0 96 96"
      role={label ? "img" : undefined} aria-label={label || undefined} aria-hidden={label ? undefined : true}>
      <g fill="none" stroke="currentColor" strokeWidth="3.2" strokeLinecap="round" strokeLinejoin="round">
        <g className="mascot-body">
          {/* The ears on top, then the round body drawn as two strokes. */}
          <path d="M31 33 Q33 24 39 30 L57 30 Q63 24 65 33" />
          <path d="M29 37 C16 50 18 78 42 85" />
          <path d="M67 37 C80 50 78 77 55 85 L50 85.6" />
          {mood === "idle" && (
            <g className="mascot-eyes" fill="currentColor" stroke="none" style={{ transform: `translate(${look.x}px, ${look.y}px)` }}>
              <circle cx="38" cy="56" r="2.6" />
              <circle cx="58" cy="56" r="2.6" />
            </g>
          )}
          {mood === "searching" && (
            <g className="mascot-eyes">
              <path d="M34 56 L42 56 M54 56 L62 56" />
            </g>
          )}
          {mood === "happy" && (
            <g>
              <path d="M33 51 L42 51 M54 51 L63 51" strokeWidth="2.6" />
              <path d="M34 55 Q38 60 42 55 M54 55 Q58 60 62 55" />
            </g>
          )}
          <path d="M43 62 Q45.5 66 48 62 Q50.5 66 53 62" strokeWidth="2.8" />
        </g>
        {mood === "searching" && (
          <g className="mascot-glass">
            <circle cx="16" cy="60" r="7" stroke="var(--accent-text, currentColor)" />
            <path d="M21 66 L25 71" stroke="var(--accent-text, currentColor)" />
            <path d="M25 74 L30 74 L30 70" strokeWidth="2.6" />
          </g>
        )}
      </g>
    </svg>
  );
}
