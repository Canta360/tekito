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
// The modes' icons, single-color in the text color as Settings' own icons
// are (assets/icons/symbols; the letters from M PLUS 1).
const modeShapes = {
  japanese: <><path fill="currentColor" d="M365.9 719.6V409.1V130H415.1V409.1V719.6ZM155.7 279.8V236.8H804.3V279.8ZM560.9 784.9Q619.6 776.7 664 750Q708.3 723.3 733.2 682.4Q758 641.6 758 592.7Q758 537.7 726.8 497Q695.6 456.4 638.5 434.2Q581.4 412 503 412Q439.7 412 384.7 430.5Q329.7 449 288.4 482.4Q247.2 515.9 224.2 560Q201.2 604.2 201.2 655.9Q201.2 698.7 217.6 725.8Q234 752.9 260.3 753.3Q283.3 753.3 319 734.4Q354.8 715.5 397.7 682Q440.6 648.6 485.8 605.2Q530.9 561.9 573 512.8Q615.1 463.8 648.8 414.1L688.5 439.5Q649.9 496.6 603.8 549.9Q557.6 603.3 509.1 649.1Q460.7 694.9 413.7 729Q366.6 763.1 325.6 781.9Q284.5 800.8 254.2 800.8Q207.8 800.4 179.7 760.6Q151.6 720.8 151.6 657.2Q151.6 595.6 177.8 543.2Q204.1 490.9 251.1 451.5Q298.1 412.1 360.9 390.1Q423.8 368.1 496.4 368.1Q592.5 368.1 662.3 395.4Q732.1 422.7 770.3 473.2Q808.4 523.7 808.4 592.7Q808.4 652.6 778.5 702.9Q748.5 753.2 695.5 786.9Q642.6 820.6 574 830Z"/></>,
  auto: <><path fill="currentColor" d="M111.7 840 370.3 180H450.2L708.3 840H643.8L410.9 231H409.1L176.2 840ZM223.7 650.1V595.9H596.7V650.1Z"/><path fill="none" stroke="currentColor" strokeWidth="36" strokeLinejoin="round" d="M800 110Q815 205 890 220Q815 235 800 330Q785 235 710 220Q785 205 800 110Z"/></>,
  mixed: <><path fill="none" stroke="currentColor" strokeWidth="45" strokeLinejoin="round" d="M480 110Q530 430 850 480Q530 530 480 850Q430 530 110 480Q430 430 480 110Z"/></>,
  direct: <><path fill="currentColor" d="M145.2 433.9V361.1H218V433.9ZM145.2 760V687.2H218V760ZM385.3 760V200H519.5Q666.8 200 740.8 270.2Q814.8 340.4 814.8 480Q814.8 619.6 740.8 689.8Q666.8 760 519.5 760ZM522.2 714Q604.3 714 657.8 688.5Q711.3 663 737.8 611Q764.3 559 764.3 480Q764.3 400.6 737.8 348.8Q711.3 297 657.8 271.5Q604.3 246 522.2 246H435.1V714Z"/></>,
};

export function ModeIcon({ id, size = 20, label }) {
  return (
    <svg className="mode-icon" width={size} height={size} viewBox="0 0 960 960" role={label ? "img" : undefined}
      aria-label={label || undefined} aria-hidden={label ? undefined : true}>
      {modeShapes[id]}
    </svg>
  );
}

// TEKITO's icon, single-color (assets/icons/symbols/tekito-outline.svg).
export function TekitoSymbol({ size = 20 }) {
  return (
    <svg className="tekito-symbol" width={size} height={size} viewBox="0 0 960 960" aria-hidden="true">
      <g fill="none" stroke="currentColor" strokeWidth="45" strokeLinecap="round" strokeLinejoin="round"><path d="M582 763A340 340 0 1 1 770 565"/><path d="M300 360H600M250 462H650M482 470L402 650"/><circle cx="709" cy="695" r="82"/></g>
    </svg>
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
