import React, { useEffect, useId, useLayoutEffect, useRef, useState } from "react";
import { symbols } from "./symbols.js";

// ---------------------------------------------------------------------------
// Icons: Material Symbols (symbols.js), drawn in the text color.

export function Symbol({ name, size = 20, className = "" }) {
  return (
    <svg className={`symbol ${className}`} width={size} height={size} viewBox="0 -960 960 960" fill="currentColor"
      aria-hidden="true" focusable="false">
      <path d={symbols[name]} />
    </svg>
  );
}

// The input modes' own icons (assets/icons), with the versions drawn for a
// dark background.
const modeImages = {
  japanese: { image: "./assets/japanese.ico", imageDark: "./assets/japanese-dark.ico" },
  mixed: { image: "./assets/mixed.ico" },
  auto: { image: "./assets/auto.ico" },
  direct: { image: "./assets/direct.ico", imageDark: "./assets/direct-dark.ico" },
};

export function ModeIcon({ id, size = 20, label }) {
  const image = modeImages[id];
  return (
    <picture className="mode-icon">
      {image.imageDark && <source media="(prefers-color-scheme: dark)" srcSet={image.imageDark} />}
      <img src={image.image} width={size} height={size} alt={label || ""} />
    </picture>
  );
}

// ---------------------------------------------------------------------------
// Cards and rows. A card holds rows with a line between them; a row is an
// icon, a name with an optional line under it, and its control. `sub` is
// what belongs to the row (a check, an expanded list), kept with it.

export function Card({ label, className = "", children, ...props }) {
  return <section className={`card ${className}`} aria-label={label} {...props}>{children}</section>;
}

function RowIcon({ icon }) {
  if (!icon) return <span className="row-icon" />;
  return <span className="row-icon">{typeof icon === "string" ? <Symbol name={icon} /> : icon}</span>;
}

export function Row({ icon, title, description, children, sub }) {
  return (
    <div className="row-block">
      <div className="row">
        <RowIcon icon={icon} />
        <span className="row-text"><span className="row-title">{title}</span>{description && <small>{description}</small>}</span>
        {children !== undefined && <span className="row-control">{children}</span>}
      </div>
      {sub}
    </div>
  );
}

// A row that opens something: a page here, or (`external`) the browser.
export function LinkRow({ icon, title, description, onClick, external = false }) {
  return (
    <div className="row-block">
      <button type="button" className="row is-link" onClick={onClick}>
        <RowIcon icon={icon} />
        <span className="row-text"><span className="row-title">{title}</span>{description && <small>{description}</small>}</span>
        <Symbol name={external ? "open_in_new" : "chevron_right"} size={18} className="row-chevron" />
      </button>
    </div>
  );
}

// A row whose choice opens below it, the current one named in the row.
export function ChoiceRow({ icon, title, description, value, options, onChange }) {
  const [open, setOpen] = useState(false);
  const id = useId();
  const current = options.find(([optionValue]) => optionValue === value) || options[0];
  return (
    <div className="row-block">
      <button type="button" className="row is-link" aria-expanded={open} aria-controls={id} onClick={() => setOpen(!open)}>
        <RowIcon icon={icon} />
        <span className="row-text"><span className="row-title">{title}</span>{description && <small>{description}</small>}</span>
        <span className="row-value">{current[1]}{current[2] && <small>{current[2]}</small>}</span>
        <Symbol name={open ? "keyboard_arrow_up" : "keyboard_arrow_down"} size={18} className="row-chevron" />
      </button>
      {open && (
        <div className="row-panel" id={id} role="radiogroup" aria-label={title}>
          {options.map(([optionValue, name, example]) => (
            <label key={optionValue} className="radio">
              <input type="radio" name={id} checked={optionValue === value} onChange={() => onChange(optionValue)} />
              <span className="radio-name">{name}</span>
              {example && <small>{example}</small>}
            </label>
          ))}
        </div>
      )}
    </div>
  );
}

// A check under a row, for a choice that belongs to it.
export function Check({ checked, onChange, label, example, disabled = false }) {
  return (
    <label className="check">
      <input type="checkbox" checked={Boolean(checked)} disabled={disabled} onChange={(event) => onChange(event.target.checked)} />
      <span>{label}</span>
      {example && <small>{example}</small>}
    </label>
  );
}

// "詳細": the finer options under each page, open or closed as last left.
export function Details({ id, title, children }) {
  const key = `tekito.details.${id}`;
  const [open, setOpen] = useState(() => {
    try {
      return window.localStorage.getItem(key) === "1";
    } catch {
      return false;
    }
  });
  const toggle = () => {
    setOpen(!open);
    try {
      window.localStorage.setItem(key, open ? "0" : "1");
    } catch {
      // Remembering is a convenience only.
    }
  };
  return (
    <div className="details">
      <button type="button" className="details-head" aria-expanded={open} onClick={toggle}>
        {title}
        <Symbol name={open ? "keyboard_arrow_up" : "keyboard_arrow_down"} size={18} />
      </button>
      {open && <div className="details-body">{children}</div>}
    </div>
  );
}

// The TEKITO lettering (the user's artwork), dark on light and light on dark.
export function Wordmark({ className = "" }) {
  return (
    <picture>
      <source media="(prefers-color-scheme: dark)" srcSet="./assets/tekito-wordmark-light.svg" />
      <img className={`wordmark ${className}`} src="./assets/tekito-wordmark-dark.svg" alt="TEKITO" />
    </picture>
  );
}

export function Heading({ children }) {
  return <h2 className="section-heading">{children}</h2>;
}

// ---------------------------------------------------------------------------
// Controls.

export function Toggle({ checked, onChange, label, disabled = false }) {
  return (
    <button type="button" role="switch" className="switch" aria-checked={Boolean(checked)} aria-label={label}
      disabled={disabled} onClick={() => onChange(!checked)}>
      <span className="switch-knob" />
    </button>
  );
}

// Segmented control: names only; the chosen one sits on a raised thumb.
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
    onChange(options[(index + delta + options.length) % options.length][0]);
    const group = event.currentTarget;
    window.requestAnimationFrame(() => group.querySelector('[aria-checked="true"]')?.focus());
  };
  return (
    <div ref={ref} className="segmented" role="radiogroup" aria-label={label} onKeyDown={onKeyDown}>
      {thumb && <span className="segmented-thumb" style={{ left: thumb.left, width: thumb.width }} />}
      {options.map(([optionValue, optionLabel]) => (
        <button key={optionValue} type="button" role="radio" aria-checked={optionValue === value}
          tabIndex={optionValue === value ? 0 : -1} onClick={() => onChange(optionValue)}>
          {optionLabel}
        </button>
      ))}
    </div>
  );
}

export function Select({ value, options, onChange, label }) {
  return (
    <span className="select">
      <select aria-label={label} value={value} onChange={(event) => onChange(Number(event.target.value))}>
        {options.map(([optionValue, name]) => <option key={optionValue} value={optionValue}>{name}</option>)}
      </select>
      <Symbol name="keyboard_arrow_down" size={18} />
    </span>
  );
}

// A slider over named stops (`stops`: [value, name]).
export function Slider({ value, stops, onChange, label }) {
  const index = Math.max(0, stops.findIndex(([stopValue]) => stopValue === value));
  return (
    <span className="slider">
      <input type="range" min={0} max={stops.length - 1} step={1} value={index} aria-label={label}
        aria-valuetext={stops[index][1]} style={{ "--fill": `${(index / (stops.length - 1)) * 100}%` }}
        onChange={(event) => onChange(stops[Number(event.target.value)][0])} />
      <span className="slider-stops">
        {stops.map(([stopValue, name], i) => (
          <span key={stopValue} className={i === index ? "is-current" : ""} style={{ "--at": i / (stops.length - 1) }}>{name}</span>
        ))}
      </span>
    </span>
  );
}

export function Button({ icon, children, variant = "", className = "", type = "button", ...props }) {
  return (
    <button type={type} className={`btn ${variant} ${className}`} {...props}>
      {icon && <Symbol name={icon} size={18} />}
      {children && <span>{children}</span>}
    </button>
  );
}

// Keys as keycaps, the same everywhere keys are shown.
export function Keycaps({ keys }) {
  return <span className="keycaps">{keys.map((key) => <kbd key={key}>{key}</kbd>)}</span>;
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
    <dialog ref={ref} className={`dialog ${wide ? "wide" : ""}`} onCancel={(event) => { event.preventDefault(); onClose(); }}
      onClick={(event) => { if (event.target === ref.current) onClose(); }}>
      {open && <div className="dialog-body"><h2>{title}</h2>{children}</div>}
    </dialog>
  );
}
