import React, { useState } from "react";
import {
  Button as FluentButton, Checkbox, Dialog as FluentDialog, DialogBody, DialogContent, DialogSurface, DialogTitle, Dropdown, Input,
  Option, ProgressBar, Radio, RadioGroup, Slider as FluentSlider, SpinButton, Switch, Tab, TabList,
} from "@fluentui/react-components";
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

// TEKITO's name drawn in the icon's line (assets/icons/symbols/tekito-wordmark-line.svg),
// for Settings' corner; About keeps the wordmark itself.
export function LineWordmark({ height = 14 }) {
  return (
    <svg className="line-wordmark" height={height} width={(height * 628) / 120} viewBox="-10 -10 628 120" role="img" aria-label="TEKITO">
      <g fill="none" stroke="currentColor" strokeWidth="11" strokeLinecap="round" strokeLinejoin="round"><path d="M0 0H96M48 0V100"/><path d="M216 0H126V100H216M126 50H206"/><path d="M246 0V100M334 0L252 60M280 40L338 100"/><path d="M368 0V100"/><path d="M398 0H494M446 0V100"/><circle cx="558" cy="50" r="50"/></g>
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

// A check under a row, for a choice that belongs to it.
export function Check({ checked, onChange, label, example, disabled = false }) {
  return (
    <div className="check">
      <Checkbox checked={Boolean(checked)} disabled={disabled} onChange={(event, data) => onChange(Boolean(data.checked))}
        label={<>{label}{example && <small className="check-example">{example}</small>}</>} />
    </div>
  );
}

// Radio choices with an example after each name: [value, name, example].
export function Radios({ value, options, onChange, label }) {
  return (
    <RadioGroup aria-label={label} value={String(value)}
      onChange={(event, data) => onChange(options.find(([optionValue]) => String(optionValue) === data.value)[0])}>
      {options.map(([optionValue, name, example]) => (
        <Radio key={String(optionValue)} value={String(optionValue)}
          label={<span className="radio-label"><span className="radio-name">{name}</span>{example && <small>{example}</small>}</span>} />
      ))}
    </RadioGroup>
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
      <FluentButton appearance="transparent" className="details-head" aria-expanded={open} onClick={toggle}
        icon={<Symbol name={open ? "keyboard_arrow_up" : "keyboard_arrow_down"} size={18} />} iconPosition="after">
        {title}
      </FluentButton>
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
// Controls: Fluent 2's own (@fluentui/react-components).

export function Toggle({ checked, onChange, label, disabled = false }) {
  return <Switch className="toggle" checked={Boolean(checked)} disabled={disabled} aria-label={label} onChange={(event, data) => onChange(data.checked)} />;
}

// A choice from a few: Fluent's dropdown. Options are [value, name] or,
// where the name is not text (keycaps), [value, name, text].
const optionKey = (value) => String(value);
export function Segmented({ value, options, onChange, label }) {
  const current = options.find(([optionValue]) => optionValue === value) || options[0];
  const text = (option) => (typeof option[1] === "string" ? option[1] : option[2] || "");
  return (
    <Dropdown className="choice" aria-label={label} value={text(current)} selectedOptions={[optionKey(current[0])]}
      button={typeof current[1] === "string" ? undefined : { children: current[1] }}
      onOptionSelect={(event, data) => {
        const chosen = options.find(([optionValue]) => optionKey(optionValue) === data.optionValue);
        if (chosen && chosen[0] !== value) onChange(chosen[0]);
      }}>
      {options.map((option) => <Option key={optionKey(option[0])} value={optionKey(option[0])} text={text(option)}>{option[1]}</Option>)}
    </Dropdown>
  );
}
export const Select = Segmented;

// Views of one list (All settings: every setting, or the changed ones).
export function Tabs({ value, options, onChange, label }) {
  return (
    <TabList aria-label={label} selectedValue={optionKey(value)}
      onTabSelect={(event, data) => onChange(options.find(([optionValue]) => optionKey(optionValue) === data.value)[0])}>
      {options.map(([optionValue, name]) => <Tab key={optionKey(optionValue)} value={optionKey(optionValue)}>{name}</Tab>)}
    </TabList>
  );
}

// A slider over named stops (`stops`: [value, name]), the names under it.
export function Slider({ value, stops, onChange, label }) {
  const index = Math.max(0, stops.findIndex(([stopValue]) => stopValue === value));
  return (
    <span className="slider">
      <FluentSlider min={0} max={stops.length - 1} step={1} value={index} aria-label={label} aria-valuetext={stops[index][1]}
        onChange={(event, data) => onChange(stops[data.value][0])} />
      <span className="slider-stops">
        {stops.map(([stopValue, name], i) => (
          <span key={stopValue} className={i === index ? "is-current" : ""} style={{ "--at": i / (stops.length - 1) }}>{name}</span>
        ))}
      </span>
    </span>
  );
}

// variant: "primary", "quiet" (subtle), "danger" (primary, in red), or
// the standard button.
export function Button({ icon, children, variant = "", className = "", type = "button", ...props }) {
  const appearance = variant === "primary" || variant === "danger" ? "primary" : variant === "quiet" ? "subtle" : "secondary";
  return (
    <FluentButton type={type} appearance={appearance} className={`${variant === "danger" ? "is-danger" : ""} ${className}`}
      icon={icon ? <Symbol name={icon} size={18} /> : undefined} {...props}>
      {children}
    </FluentButton>
  );
}

// A button that is only an icon, named for screen readers and on hover.
export function IconButton({ icon, label, className = "", ...props }) {
  return (
    <FluentButton appearance="subtle" size="small" className={className} icon={<Symbol name={icon} size={18} />}
      aria-label={label} title={label} {...props} />
  );
}

// Fluent's text box; `before` and `after` sit inside it.
export function TextBox({ before, after, ...props }) {
  return <Input contentBefore={before} contentAfter={after} {...props} />;
}

export { SpinButton };

// Work under way, its end not known.
export function Busy() {
  return <ProgressBar className="progress" thickness="medium" />;
}

// Keys as keycaps, the same everywhere keys are shown.
export function Keycaps({ keys }) {
  return <span className="keycaps">{keys.map((key) => <kbd key={key}>{key}</kbd>)}</span>;
}

export function Dialog({ open, title, onClose, children, wide = false }) {
  return (
    <FluentDialog open={open} onOpenChange={(event, data) => { if (!data.open) onClose(); }}>
      <DialogSurface className={wide ? "dialog-wide" : "dialog"}>
        <DialogBody>
          <DialogTitle>{title}</DialogTitle>
          <DialogContent className="dialog-content">{children}</DialogContent>
        </DialogBody>
      </DialogSurface>
    </FluentDialog>
  );
}
