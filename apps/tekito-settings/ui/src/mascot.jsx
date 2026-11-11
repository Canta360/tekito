// Home's companion: a cat drawn in the same single line as Settings' icons
// (the text color, the accent for its glass). Sitting, it blinks and its
// tail sways; looking (`searching`), it peers left and right through a
// magnifying glass. Still when the user asks Windows for less motion.
import React from "react";

export function Mascot({ searching = false, size = 88, label }) {
  return (
    <svg className={`mascot ${searching ? "is-searching" : ""}`} width={size} height={size} viewBox="0 0 96 96"
      role={label ? "img" : undefined} aria-label={label || undefined} aria-hidden={label ? undefined : true}>
      <g fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round">
        <path className="mascot-tail" d="M62 80 C78 80 86 70 82 58 C80 52 74 52 74 58" />
        <path d="M34 84 C30 70 34 58 48 58 C62 58 66 70 62 84 Z" />
        <path d="M40 84 L40 76 M56 84 L56 76" />
        <g className="mascot-head">
          <path d="M28 44 L30 22 L41 31 Q48 29 55 31 L66 22 L68 44 Q68 60 48 60 Q28 60 28 44 Z" />
          <g className="mascot-eyes" fill="currentColor" stroke="none">
            <ellipse cx="40" cy="44" rx="2.4" ry="3" />
            <ellipse cx="56" cy="44" rx="2.4" ry="3" />
          </g>
          <path d="M45 50 Q48 53 51 50" />
          <path d="M24 46 L34 47 M24 52 L34 50 M72 46 L62 47 M72 52 L62 50" strokeWidth="2" />
        </g>
        <g className="mascot-glass">
          <circle cx="74" cy="38" r="7.5" stroke="var(--accent-text, currentColor)" />
          <path d="M79.5 43.5 L86 50" stroke="var(--accent-text, currentColor)" />
        </g>
      </g>
    </svg>
  );
}
