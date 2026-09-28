import React, { useEffect, useLayoutEffect, useRef, useState } from "react";

// ---------------------------------------------------------------------------
// Icons: 24px line icons drawn with currentColor.

const iconPaths = {
  general: <><path d="M4 7h9M17 7h3M4 17h3M11 17h9" /><circle cx="15" cy="7" r="2" /><circle cx="9" cy="17" r="2" /></>,
  typing: <><rect x="3" y="6" width="18" height="12" rx="3" /><path d="M7 10h.01M10.5 10h.01M14 10h.01M17 10h.01M8 14h8" /></>,
  dictionary: <><path d="M5 5.5A2.5 2.5 0 0 1 7.5 3H19v14H7.5A2.5 2.5 0 0 0 5 19.5z" /><path d="M5 19.5A2.5 2.5 0 0 0 7.5 22H19v-5" /></>,
  data: <><ellipse cx="12" cy="6" rx="7" ry="3" /><path d="M5 6v6c0 1.7 3.1 3 7 3s7-1.3 7-3V6" /><path d="M5 12v6c0 1.7 3.1 3 7 3s7-1.3 7-3v-6" /></>,
  about: <><circle cx="12" cy="12" r="9" /><path d="M12 11v5M12 7.6h.01" /></>,
  spelling: <><path d="M3.5 16 7.5 6l4 10M5 12.5h5" /><path d="m13.5 14 2.5 2.5L21 11" /></>,
  misspellings: <><path d="M4 6h10M4 11h8M4 16h5" /><path d="m13 16 2.5 2.5L20.5 13" /></>,
  context: <><path d="M5 5h14a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H10l-5 4v-4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z" /><path d="M8.5 10.5h7" /></>,
  completion: <><path d="M4 12h9" /><path d="m10 8 4 4-4 4" /><path d="M19 5v14" /></>,
  candidates: <><rect x="4" y="3.5" width="16" height="17" rx="5" /><rect x="7" y="9.5" width="10" height="5" rx="2.5" /><path d="M8 6.8h6M8 17.3h5" /></>,
  japanese: <text x="12" y="17.2" textAnchor="middle" fontSize="14" fontWeight="600" fill="currentColor" stroke="none">あ</text>,
  english: <text x="12" y="17.4" textAnchor="middle" fontSize="15" fontWeight="600" fill="currentColor" stroke="none">A</text>,
  search: <><circle cx="11" cy="11" r="6" /><path d="m20 20-4.5-4.5" /></>,
  plus: <path d="M12 5v14M5 12h14" />,
  edit: <><path d="M4 20h4L19 9l-4-4L4 16z" /><path d="m13.5 6.5 4 4" /></>,
  trash: <><path d="M4 7h16M10 11v6M14 11v6" /><path d="M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12M9 7V4h6v3" /></>,
  import: <><path d="M12 4v11M7 10l5 5 5-5" /><path d="M5 20h14" /></>,
  export: <><path d="M12 15V4M7 9l5-5 5 5" /><path d="M5 20h14" /></>,
  copy: <><rect x="8" y="8" width="12" height="12" rx="2.5" /><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2" /></>,
  folder: <path d="M4 7a2 2 0 0 1 2-2h4l2 2h6a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2z" />,
  license: <><path d="M7 3h7l5 5v13H7z" /><path d="M14 3v5h5M10 13h6M10 17h4" /></>,
  shield: <><path d="M12 3 5 6v5c0 4.4 3 8.2 7 10 4-1.8 7-5.6 7-10V6z" /><path d="m9 12 2 2 4-4" /></>,
  learning: <><path d="M12 3a6 6 0 0 0-3.5 10.9V16h7v-2.1A6 6 0 0 0 12 3z" /><path d="M9.5 19.5h5M10.5 22h3" /></>,
  globe: <><circle cx="12" cy="12" r="9" /><path d="M3 12h18M12 3c2.5 2.6 3.8 5.6 3.8 9s-1.3 6.4-3.8 9c-2.5-2.6-3.8-5.6-3.8-9S9.5 5.6 12 3z" /></>,
  arrow: <path d="M5 12h13M13 7l5 5-5 5" />,
};

export function Icon({ name, size = 20, className = "" }) {
  return (
    <svg className={`icon ${className}`} width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor"
      strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true" focusable="false">
      {iconPaths[name]}
    </svg>
  );
}

// ---------------------------------------------------------------------------
// Background: one soft pool of the accent color that eases toward the
// pointer, so the glass panes show light moving beneath them. Nothing else
// in the background carries color.

export function Backdrop() {
  const orb = useRef(null);
  useEffect(() => {
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return undefined;
    let target = { x: window.innerWidth * 0.62, y: window.innerHeight * 0.38 };
    const current = { ...target };
    let frame = 0;
    const step = () => {
      current.x += (target.x - current.x) * 0.06;
      current.y += (target.y - current.y) * 0.06;
      if (orb.current) orb.current.style.transform = `translate(${current.x}px, ${current.y}px) translate(-50%, -50%)`;
      frame = Math.abs(target.x - current.x) + Math.abs(target.y - current.y) > 0.5 ? requestAnimationFrame(step) : 0;
    };
    const onMove = (event) => {
      target = { x: event.clientX, y: event.clientY };
      if (!frame) frame = requestAnimationFrame(step);
    };
    window.addEventListener("pointermove", onMove, { passive: true });
    frame = requestAnimationFrame(step);
    return () => {
      window.removeEventListener("pointermove", onMove);
      cancelAnimationFrame(frame);
    };
  }, []);
  return <div className="backdrop" aria-hidden="true"><span className="orb" ref={orb} /></div>;
}
// ---------------------------------------------------------------------------
// Glass surfaces.

export function Glass({ as: Tag = "section", className = "", children, ...props }) {
  return <Tag className={`glass ${className}`} {...props}>{children}</Tag>;
}

// Square glass tile with a pool of accent light behind it. The light shows
// only when the tile is on (or selected), and leans toward the pointer.
// `imageDark`, when given, is the image for a dark background.
export function Tile({ on, onPress, icon, image, imageDark, art, title, description, status, className = "", ...props }) {
  const ref = useRef(null);
  const onPointerMove = (event) => {
    const rect = ref.current?.getBoundingClientRect();
    if (!rect) return;
    ref.current.style.setProperty("--lx", `${((event.clientX - rect.left) / rect.width) * 100}%`);
    ref.current.style.setProperty("--ly", `${((event.clientY - rect.top) / rect.height) * 100}%`);
  };
  const onPointerLeave = () => {
    ref.current?.style.removeProperty("--lx");
    ref.current?.style.removeProperty("--ly");
  };
  return (
    <button ref={ref} type="button" className={`tile ${on ? "is-on" : ""} ${className}`}
      aria-pressed={on} onClick={onPress} onPointerMove={onPointerMove} onPointerLeave={onPointerLeave} {...props}>
      <span className="tile-light" />
      <span className="tile-glass">
        <span className="tile-top">
          {art || (image ? (
            <picture>
              {imageDark && <source media="(prefers-color-scheme: dark)" srcSet={imageDark} />}
              <img className="tile-image" src={image} alt="" />
            </picture>
          ) : <Icon name={icon} size={24} className="tile-icon" />)}
          {status !== undefined && <span className="tile-status"><span className="dot" />{status}</span>}
        </span>
        <span className="tile-text">
          <b>{title}</b>
          {description && <small>{description}</small>}
        </span>
      </span>
    </button>
  );
}

// ---------------------------------------------------------------------------
// Controls.

export function Toggle({ checked, onChange, label, disabled = false }) {
  return (
    <button type="button" role="switch" className="switch" aria-checked={Boolean(checked)} aria-label={label}
      disabled={disabled} onClick={() => onChange(!checked)}>
      <span className="switch-light" />
      <span className="switch-knob" />
    </button>
  );
}

// Pill-shaped segmented control; the selected option sits in a recessed
// pill that slides between segments.
// Each choice is as wide as its label; the thumb is measured to sit under
// the chosen one.
export function Segmented({ value, options, onChange, label }) {
  const index = Math.max(0, options.findIndex(([optionValue]) => optionValue === value));
  const ref = useRef(null);
  const [thumb, setThumb] = useState(null);
  useLayoutEffect(() => {
    const group = ref.current;
    if (!group) return undefined;
    const measure = () => {
      const chosen = group.querySelectorAll("button")[index];
      if (chosen) setThumb({ left: chosen.offsetLeft, width: chosen.offsetWidth });
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(group);
    return () => observer.disconnect();
  }, [index, options.length]);
  const onKeyDown = (event) => {
    const delta = event.key === "ArrowRight" ? 1 : event.key === "ArrowLeft" ? -1 : 0;
    if (!delta) return;
    event.preventDefault();
    const next = options[(index + delta + options.length) % options.length][0];
    onChange(next);
    const group = event.currentTarget;
    window.requestAnimationFrame(() => group.querySelector('[aria-checked="true"]')?.focus());
  };
  return (
    <div ref={ref} className="segmented" role="radiogroup" aria-label={label} onKeyDown={onKeyDown}>
      <span className={`segmented-thumb ${thumb ? "" : "is-unmeasured"}`} style={thumb ? { left: thumb.left, width: thumb.width } : undefined} />
      {options.map(([optionValue, optionLabel]) => (
        <button key={optionValue} type="button" role="radio" aria-checked={optionValue === value}
          tabIndex={optionValue === value ? 0 : -1} onClick={() => onChange(optionValue)}>
          {optionLabel}
        </button>
      ))}
    </div>
  );
}

export function Button({ icon, children, variant = "", className = "", type = "button", ...props }) {
  return (
    <button type={type} className={`btn ${variant} ${className}`} {...props}>
      {icon && <Icon name={icon} size={17} />}
      {children && <span>{children}</span>}
    </button>
  );
}

// Keys as keycaps, the same everywhere keys are shown.
export function Keycaps({ keys }) {
  return <span className="keycaps">{keys.map((key) => <kbd key={key}>{key}</kbd>)}</span>;
}

// `stacked` puts the control under the text, for wide controls.
export function Row({ title, description, stacked = false, children }) {
  return (
    <div className={`row ${stacked ? "stacked" : ""}`}>
      <span className="row-text"><b>{title}</b>{description && <small>{description}</small>}</span>
      <span className="row-control">{children}</span>
    </div>
  );
}

export function Dialog({ open, title, onClose, children, wide = false }) {
  const ref = useRef(null);
  useEffect(() => {
    const dialog = ref.current;
    if (!dialog) return;
    if (open && !dialog.open) dialog.showModal();
    if (!open && dialog.open) dialog.close();
  }, [open]);
  return (
    <dialog ref={ref} className={`dialog glass ${wide ? "wide" : ""}`} onCancel={(event) => { event.preventDefault(); onClose(); }}
      onClick={(event) => { if (event.target === ref.current) onClose(); }}>
      {open && <div className="dialog-body"><h2>{title}</h2>{children}</div>}
    </dialog>
  );
}
