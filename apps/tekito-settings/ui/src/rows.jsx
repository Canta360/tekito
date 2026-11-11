// The rows Settings is made of that are more than a switch: shared by the
// pages, Home's answers and All settings (catalog.jsx).
import React, { useState } from "react";
import { useText } from "./i18n.js";
import { Checkbox } from "@fluentui/react-components";
import { Busy, Button, Check, Dialog, IconButton, Keycaps, ModeIcon, Radios, Row, Segmented, SpinButton, Symbol, Tabs, TextBox, Toggle } from "./ui.jsx";

// The modes a setting applies to, as their icons after its name.
export function ModeBadges({ ids }) {
  const t = useText();
  if (!ids.length) return null;
  return (
    <span className="mode-badges" role="img" aria-label={ids.map((id) => t.modeNames[id]).join("・")}>
      {ids.map((id) => <ModeIcon key={id} id={id} size={14} />)}
    </span>
  );
}

// Two modes' switches for the same thing, linked (one switch sets both)
// until the chain is opened; values that differ show them apart.
const unlinkedKey = (id) => `tekito.unlinked.${id}`;
const readUnlinked = (id) => {
  try {
    return window.localStorage.getItem(unlinkedKey(id)) === "1";
  } catch {
    return false;
  }
};
const writeUnlinked = (id, value) => {
  try {
    window.localStorage.setItem(unlinkedKey(id), value ? "1" : "0");
  } catch {
    // Kept apart only while the page is open.
  }
};

export function LinkedToggle({ id, icon, title, description, keys, settings, setSetting, on, multi, sub }) {
  const t = useText();
  const l = t.linked;
  const modes = Object.keys(keys).filter((mode) => on[mode]);
  const [unlinked, setUnlinked] = useState(() => readUnlinked(id));
  const heading = <>{title}{multi && <ModeBadges ids={modes} />}</>;
  if (modes.length < 2) {
    const key = keys[modes[0]];
    return (
      <Row icon={icon} title={heading} description={description} sub={sub}>
        <Toggle label={title} checked={settings[key]} onChange={(value) => setSetting(key, value)} />
      </Row>
    );
  }
  const [first, second] = modes;
  const differ = Boolean(settings[keys[first]]) !== Boolean(settings[keys[second]]);
  const apart = unlinked || differ;
  const setBoth = async (value) => {
    await setSetting(keys[first], value);
    await setSetting(keys[second], value);
  };
  const link = async () => {
    writeUnlinked(id, false);
    setUnlinked(false);
    if (differ) await setSetting(keys[second], Boolean(settings[keys[first]]));
  };
  const unlink = () => {
    writeUnlinked(id, true);
    setUnlinked(true);
  };
  // Said in words under the row: set apart for each mode, or the same again.
  return (
    <Row icon={icon} title={heading} description={description}
      sub={(
        <>
          {apart && (
            <div className="per-mode">
              {modes.map((mode) => (
                <div key={mode} className="per-mode-row">
                  <ModeIcon id={mode} size={16} />
                  <span>{t.modeNames[mode]}</span>
                  <Toggle label={`${title}（${t.modeNames[mode]}）`} checked={settings[keys[mode]]} onChange={(value) => setSetting(keys[mode], value)} />
                </div>
              ))}
            </div>
          )}
          {sub}
          <div className="linked-action">
            <Button variant="quiet" size="small" icon={apart ? "link" : "link_off"} onClick={apart ? link : unlink}>
              {apart ? l.link(t.modeNames[first], t.modeNames[second]) : l.unlink(t.modeNames[first], t.modeNames[second])}
            </Button>
          </div>
        </>
      )}>
      {!apart && <Toggle label={title} checked={settings[keys[first]]} onChange={setBoth} />}
    </Row>
  );
}

// A row whose choices open below it; `value` names the current ones.
export function ExpandRow({ icon, title, description, value, children }) {
  const [open, setOpen] = useState(false);
  return (
    <div className="row-block">
      <button type="button" className="row is-link" aria-expanded={open} onClick={() => setOpen(!open)}>
        <span className="row-icon"><Symbol name={icon} /></span>
        <span className="row-text"><span className="row-title">{title}</span>{description && <small>{description}</small>}</span>
        <span className="row-value">{value}</span>
        <Symbol name={open ? "keyboard_arrow_up" : "keyboard_arrow_down"} size={18} className="row-chevron" />
      </button>
      {open && <div className="row-panel">{children}</div>}
    </div>
  );
}

// 全角と半角: letters, symbols and the space, in one row.
export function WidthsRow({ settings, setSetting, badges = null }) {
  const t = useText();
  const w = t.widths;
  const half = t.japanese.space.half;
  const full = t.japanese.space.full;
  const parts = [["japaneseLetterWidth", w.letters], ["japaneseSymbolWidth", w.symbols], ["japaneseSpaceWidth", w.space]];
  const isHalf = (key) => settings[key] === 1;
  return (
    <ExpandRow icon="abc" title={<>{w.title}{badges}</>} description={w.description}
      value={parts.map(([key, name]) => `${name} ${isHalf(key) ? half : full}`).join("・")}>
      {parts.map(([key, name]) => (
        <div key={key} className="panel-line">
          <span>{name}</span>
          <Segmented label={name} value={isHalf(key) ? 1 : 0} options={[[1, half], [0, full]]} onChange={(value) => setSetting(key, value)} />
        </div>
      ))}
    </ExpandRow>
  );
}

// 日付・記号・計算: the extra candidates, each with its own check.
export function SpecialRow({ settings, setSetting }) {
  const t = useText();
  const g = t.general;
  const sp = t.special;
  const today = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  const japaneseDate = `${today.getFullYear()}/${pad(today.getMonth() + 1)}/${pad(today.getDate())}`;
  const englishDate = today.toLocaleDateString("en-US", { month: "long", day: "numeric", year: "numeric" });
  const parts = [
    ["dateConversion", sp.parts[0], g.date.description(japaneseDate, englishDate)],
    ["symbolConversion", sp.parts[1], g.symbols.description],
    ["calculatorEnabled", sp.parts[2], g.calculator.description],
  ];
  const shown = parts.filter(([key]) => settings[key]).map(([, name]) => name);
  return (
    <ExpandRow icon="calculate" title={sp.title} value={shown.length ? shown.join("・") : sp.none}>
      {parts.map(([key, name, example]) => (
        <Check key={key} label={name} example={example} checked={settings[key]} onChange={(value) => setSetting(key, value)} />
      ))}
    </ExpandRow>
  );
}

// The keyboard's own switch key (UserSettings::toggleKey 1).
const ownSwitchKey = (japaneseKeyboard, t) => (japaneseKeyboard ? [t.keyNames.hankaku] : ["Alt", "`"]);
export const switchKeyCaps = (toggleKey, japaneseKeyboard, t) =>
  toggleKey === 1 ? ownSwitchKey(japaneseKeyboard, t)
  : toggleKey === 2 ? ["Ctrl", "Space"]
  : toggleKey === 3 ? ["Ctrl", "Shift", "Space"]
  : null;

// The keys, by mode: [id, name, keycaps]. The ids are what AI answers name.
export function keyGroups(t, { japaneseKeyboard, settings, japanese }) {
  const general = [];
  const switchKey = switchKeyCaps(settings.toggleKey, japaneseKeyboard, t);
  if (switchKey) general.push(["switch", japanese ? t.modeKeys.switch : t.modeKeys.switchEnglish, switchKey]);
  if (japanese && japaneseKeyboard) {
    general.push(["henkan", t.modeKeys.henkan, [t.keyNames.henkan]], ["muhenkan", t.modeKeys.muhenkan, [t.keyNames.muhenkan]],
      ["hiragana", t.modeKeys.hiragana, [t.keyNames.hiragana]]);
  }
  general.push(["taskbar", t.modeKeys.taskbar, [t.modeKeys.taskbarButton]]);
  const k = t.japanese.keys;
  const japaneseKeys = [["convert", k.convert, ["Space"]], ["phrases", k.phrases, ["←", "→"]], ["resize", k.resize, ["Shift", "←", "→"]],
    ["predictions", k.predictions, ["Tab", "↓"]], ["kana", k.kana, ["F6", "F7", "F8"]], ["letters", k.letters, ["F9", "F10"]],
    ["ctrlForms", k.ctrlForms, ["Ctrl", "U I O P T"]], ["english", k.english, ["Shift", "A-Z"]], ["back", k.back, ["Esc"]],
    ["undo", k.undo, ["Ctrl", "Backspace"]], ["forget", k.forget, ["Ctrl", "Delete"]]];
  const a = t.auto.keys;
  const autoKeys = [["next", a.next, ["Space"]], ["previous", a.previous, ["Shift", "Space"]], ["browse", a.browse, ["Tab", "↑", "↓"]],
    ["undo", a.undo, ["Backspace"]], ["keep", a.keep, ["Esc"]]];
  return [
    ["general", t.pages.general, general],
    ...(japanese ? [["japanese", t.modeNames.japanese, japaneseKeys]] : []),
    ["auto", t.modeNames.auto, autoKeys],
  ];
}

export function KeyList(props) {
  const t = useText();
  return keyGroups(t, props).map(([id, name, rows]) => (
    <React.Fragment key={id}>
      <h3 className="key-heading">{name}</h3>
      <ul className="list key-list">
        {rows.map(([key, label, caps]) => <li key={key}><span className="text">{label}</span><Keycaps keys={caps} /></li>)}
      </ul>
    </React.Fragment>
  ));
}

// The modes in UserSettings::switchOrder's digits, and their switchModes bits.
export const orderDigits = { 1: "japanese", 2: "mixed", 3: "auto", 4: "direct" };
export const modeBits = { japanese: 1, mixed: 2, auto: 4, direct: 8 };
export const orderIds = (order) => String(order).split("").map((digit) => orderDigits[digit]).filter(Boolean);
export const orderValue = (ids) => Number(ids.map((id) => Object.keys(orderDigits).find((digit) => orderDigits[digit] === id)).join(""));

// Settings found only by searching for them by name: [page, name, under
// 詳細, names that find it, key]. Once found they stay on the page.
export const hiddenSettings = (t) => [
  ["general", t.general.rainbow.title, true, ["rainbow", "レインボー", "にじ", "虹"], "rainbow"],
  ["general", t.general.cat.title, true, ["cat", "ねこ", "猫", "ネコ"], "cat"],
];
export const revealHidden = (key) => {
  try {
    window.localStorage.setItem(`tekito.found.${key}`, "1");
  } catch {
    // Showing it again later is a convenience only.
  }
};
export const wasFound = (key) => {
  try {
    return window.localStorage.getItem(`tekito.found.${key}`) === "1";
  } catch {
    return false;
  }
};

// The switch key's round: the modes, each on or off, dragged into order.
export function SwitchOrder({ settings, setSetting, japaneseInstalled, japaneseInstalling, addJapanese, japaneseError }) {
  const t = useText();
  const g = t.general;
  const m = t.modes;
  // Every mode is listed. Checked: Japanese and Mix are used (off, they are
  // not there at all); Auto and Direct, always there, go round with the
  // switch key. Without the Japanese data, Japanese and Mix offer to add it.
  const enabledKey = { japanese: "japaneseEnabled", mixed: "mixedTypingEnabled" };
  const available = (id) => japaneseInstalled || !enabledKey[id];
  const enabled = (id) => !enabledKey[id] || (japaneseInstalled && settings[enabledKey[id]] !== false && Boolean(settings[enabledKey[id]] ?? true));
  const inRound = (id) => (settings.switchModes & modeBits[id]) !== 0;
  const checked = (id) => enabled(id) && inRound(id);
  const all = orderIds(settings.switchOrder);
  const ids = all.filter(available);
  const missing = all.filter((id) => !available(id));
  const round = ids.filter(checked);
  const [dragging, setDragging] = useState(null);
  const [target, setTarget] = useState(null);
  const check = async (id, value) => {
    if (enabledKey[id]) await setSetting(enabledKey[id], value);
    if (value && !inRound(id)) await setSetting("switchModes", settings.switchModes | modeBits[id]);
    if (!value && !enabledKey[id]) await setSetting("switchModes", settings.switchModes & ~modeBits[id]);
  };
  // Modes not listed keep their place in the order.
  const move = (id, to) => {
    const from = ids.indexOf(id);
    if (from < 0 || to < 0 || to >= ids.length || from === to) return;
    const moved = [...ids];
    moved.splice(from, 1);
    moved.splice(to, 0, id);
    let next = 0;
    setSetting("switchOrder", orderValue(all.map((other) => (available(other) ? moved[next++] : other))));
  };
  return (
    <Row icon="swap_horiz" title={g.order.title} description={m.orderDescription}
      sub={(
        <ul className="order" aria-label={g.order.title}>
          {ids.map((id, index) => (
            <li key={id} draggable className={`${checked(id) ? "" : "is-off"} ${dragging === id ? "is-dragging" : ""} ${target === id ? "is-target" : ""}`}
              onDragStart={(event) => { setDragging(id); event.dataTransfer.effectAllowed = "move"; }}
              onDragOver={(event) => { event.preventDefault(); setTarget(id); }}
              onDragLeave={() => setTarget(null)}
              onDrop={(event) => { event.preventDefault(); move(dragging, index); setDragging(null); setTarget(null); }}
              onDragEnd={() => { setDragging(null); setTarget(null); }}
              onKeyDown={(event) => {
                if (!event.altKey) return;
                if (event.key === "ArrowUp") { event.preventDefault(); move(id, index - 1); }
                if (event.key === "ArrowDown") { event.preventDefault(); move(id, index + 1); }
              }}>
              <span className="handle" aria-hidden="true"><Symbol name="drag_indicator" size={18} /></span>
              <Checkbox aria-label={t.modeNames[id]} checked={checked(id)} onChange={(event, data) => check(id, Boolean(data.checked))} />
              <ModeIcon id={id} size={20} />
              <span className="name">{t.modeNames[id]}{id === "mixed" && <small className="order-note">{m.mixed}</small>}</span>
              <IconButton icon="keyboard_arrow_up" label={g.order.moveUp(t.modeNames[id])} disabled={index === 0} onClick={() => move(id, index - 1)} />
              <IconButton icon="keyboard_arrow_down" label={g.order.moveDown(t.modeNames[id])} disabled={index === ids.length - 1}
                onClick={() => move(id, index + 1)} />
            </li>
          ))}
          {missing.length > 0 && (
            <li className="order-missing">
              <span className="handle" />
              {missing.map((id) => <ModeIcon key={id} id={id} size={20} />)}
              <span className="name">
                {missing.map((id) => t.modeNames[id]).join("・")}
                <small className={`order-note ${japaneseError ? "problem" : ""}`}>{japaneseError || (addJapanese ? m.needsJapanese : m.bySetup)}</small>
              </span>
              {japaneseInstalling
                ? <span className="row-status"><Busy />{m.adding}</span>
                : addJapanese && <Button variant="primary" onClick={addJapanese}>{m.add}</Button>}
            </li>
          )}
        </ul>
      )}>
      <span className="cycle" aria-hidden="true">
        {round.map((id) => <React.Fragment key={id}><ModeIcon id={id} size={22} /><Symbol name="chevron_right" size={16} /></React.Fragment>)}
        <Symbol name="repeat" size={18} />
      </span>
    </Row>
  );
}

export function ExcludedApps({ settings, runAction }) {
  const t = useText();
  const e = t.general.excluded;
  const [name, setName] = useState("");
  const add = async (event) => {
    event.preventDefault();
    if (!name.trim()) return;
    const response = await runAction("excludedApps.add", { name: name.trim() });
    if (response.ok) setName("");
  };
  return (
    <Row icon="block" title={e.title} description={e.description}
        sub={(
          <>
            <ul className="list" aria-label={e.title}>
              {settings.excludedApps.map((app) => (
                <li key={app}>
                  <span className="text">{app}</span>
                  <IconButton icon="close" label={e.remove(app)} onClick={() => runAction("excludedApps.remove", { name: app })} />
                </li>
              ))}
            </ul>
            <form className="inline-form" onSubmit={add}>
              <TextBox aria-label={e.placeholder} placeholder={e.placeholder} value={name} onChange={(event) => setName(event.target.value)} />
              <Button type="submit" disabled={!name.trim()}>{t.add}</Button>
            </form>
          </>
        )}>
        <Button onClick={() => runAction("excludedApps.browse", {})}>{e.choose}</Button>
      </Row>
  );
}

// The kanji slider's stops and the settings each stands for.
export const kanjiStops = [1, 2, 3, 4, 5, 6, 7, 8];
export function kanjiStop(settings) {
  if (settings.japaneseKanjiRange === 3) return Math.min(Math.max(settings.japaneseKanjiGrade, 1), 6);
  if (settings.japaneseKanjiRange === 0) return 8;
  return 7;
}

// 数字の変換: the style, and the checks that belong to numbers.
export function NumbersRow({ settings, setSetting, options, onChange, badges = null }) {
  const t = useText();
  const j = t.japanese;
  const [open, setOpen] = useState(false);
  const style = Math.min(Math.max(settings.japaneseNumberStyle, 1), 5);
  const current = options[style - 1];
  return (
    <div className="row-block">
      <button type="button" className="row is-link" aria-expanded={open} onClick={() => setOpen(!open)}>
        <span className="row-icon"><Symbol name="pin" /></span>
        <span className="row-text"><span className="row-title">{j.numbers.title}{badges}</span><small>{j.numbers.description}</small></span>
        <span className="row-value">{current[1]}<small>{current[2]}</small></span>
        <Symbol name={open ? "keyboard_arrow_up" : "keyboard_arrow_down"} size={18} className="row-chevron" />
      </button>
      {open && (
        <div className="row-panel">
          <Radios label={j.numbers.title} value={style} options={options} onChange={onChange} />
          <span className="row-panel-line" />
          <Check label={j.numbers.rough} example={j.numbers.roughExample} checked={settings.japaneseRoughNumbersKanji}
            onChange={(value) => setSetting("japaneseRoughNumbersKanji", value)} />
          <Check label={j.numbers.other} example={j.numbers.otherExample} checked={settings.numberConversion}
            onChange={(value) => setSetting("numberConversion", value)} />
        </div>
      )}
    </div>
  );
}

// Bytes as the download dialog states them (230 MB, 1.3 GB).
export const downloadSize = (bytes) => (bytes >= 1e9 ? `${(bytes / 1e9).toFixed(1)} GB` : `${Math.round(bytes / 1e6)} MB`);

export function AiModelRow({ settings, setSetting, runAction, setConfirm, aiModels, badges = null }) {
  const t = useText();
  const a = t.ai;
  const keys = ["", "light", "strong"];
  const downloading = aiModels.downloading || 0;
  // The model chosen, while it and what runs it are there (an older
  // model's format is not: it is downloaded again).
  const chosen = settings.japaneseSentenceModel || 0;
  const current = chosen > 0 && aiModels[keys[chosen]]?.installed && aiModels.runtime?.installed ? chosen : 0;
  // Where it runs: the GPU when the PC has one, the NPU only when it has one.
  const devices = [[0, a.device.auto], [1, "CPU"]];
  if ((aiModels.gpus || []).length > 0) devices.push([2, "GPU"]);
  if (aiModels.npu) devices.push([3, "NPU"]);
  const saved = settings.japaneseAiDevice || 0;
  // The GPU or the NPU only once Windows ML is there: a choice saved
  // without it (an older version's GPU) runs on the processor.
  const device = devices.some(([value]) => value === saved) && (saved < 2 || aiModels.runtime?.windowsMl) ? saved : 0;
  const busy = aiModels.npuPreparing || aiModels.runtime?.windowsMlDownloading;
  const chooseDevice = (value) => {
    if (busy) return;
    // The GPU and the NPU need Windows ML: downloaded once the user agrees
    // to Microsoft's terms, then used.
    if (value >= 2 && !aiModels.runtime?.windowsMl) {
      setConfirm({
        title: a.device.dialogTitle,
        message: a.device.dialogBody(downloadSize(aiModels.runtime?.windowsMlBytes || 0)),
        confirmLabel: a.download,
        action: () => runAction("aiRuntime.windowsMl", { device: value }),
        link: { label: a.device.terms, keep: true, action: () => runAction("windowsMl.terms", {}) },
      });
      return;
    }
    setSetting("japaneseAiDevice", value);
    if (value === 3) runAction("aiNpu.prepare", {});
  };
  const choose = (value) => {
    if (downloading) return;
    if (value === 0 || (aiModels[keys[value]]?.installed && aiModels.runtime?.installed)) {
      setSetting("japaneseSentenceModel", value);
      return;
    }
    setConfirm({
      title: a.dialogTitle(a.names[value]),
      message: a.dialogBody(downloadSize((aiModels[keys[value]]?.bytes || 0) +
        (aiModels.runtime?.installed ? 0 : aiModels.runtime?.bytes || 0)), a.memory[value]),
      confirmLabel: a.download,
      action: () => runAction("aiModel.download", { model: value }),
    });
  };
  return (
    <>
    <Row icon="wand_stars" title={<>{a.title}{badges}</>}
      description={(
        <>
          {downloading ? "" : aiModels.error || a.description}
          {downloading > 0 && <span className="row-status"><Busy />{a.downloading}</span>}
        </>
      )}
      sub={current > 0 && !downloading && (
        <div className="row-sub-action">
          <Button variant="quiet" onClick={() => runAction("aiModel.remove", { model: current }, a.removed)}>{a.remove}</Button>
        </div>
      )}>
      <Segmented label={a.title} value={downloading || current} options={[[0, a.off], [1, a.names[1]], [2, a.names[2]]]}
        onChange={choose} />
    </Row>
    {current > 0 && (
      <Row icon="memory" title={a.device.title}
        description={(
          <>
            {busy ? "" : aiModels.npuError || (device === 3 ? a.device.npuFirst : a.device.description)}
            {busy && <span className="row-status"><Busy />
              {aiModels.runtime?.windowsMlDownloading ? a.device.downloading : a.device.preparing}</span>}
          </>
        )}>
        <Segmented label={a.device.title} value={device} options={devices} onChange={chooseDevice} />
      </Row>
    )}
    </>
  );
}

// Postal codes to addresses: Japan Post's data, fetched only once the user
// agrees to it, or taken from a file they downloaded.
export function PostalCodes({ postalCodes, runAction, setConfirm }) {
  const t = useText();
  const p = t.japanese.postal;
  const [busy, setBusy] = useState(false);
  const fetchData = async (type) => {
    setBusy(true);
    await runAction(type, {}, p.added);
    setBusy(false);
  };
  const turn = (value) => {
    if (!value) {
      runAction("postalCodes.remove", {}, p.removed);
      return;
    }
    setConfirm({
      title: p.dialogTitle,
      message: p.dialogBody,
      confirmLabel: p.download,
      action: () => fetchData("postalCodes.download"),
      link: { label: p.useFile, action: () => fetchData("postalCodes.browse") },
    });
  };
  const [year, month] = String(postalCodes.version || "").split(".");
  return (
    <Row icon="home_pin" title={p.title}
      description={(
        <>
          {postalCodes.installed && !busy ? p.installed(year, month, postalCodes.count) : p.description}
          {busy && <span className="row-status"><Busy />{p.downloading}</span>}
        </>
      )}>
      <Toggle label={p.title} checked={postalCodes.installed || busy} disabled={busy} onChange={turn} />
    </Row>
  );
}

// 1ページの候補数: the usual numbers, or any from 3 to 20 (UserSettings::
// kMinCandidateRows..kMaxCandidateRows).
export const candidateRowsRange = [3, 20];
const standardRows = [0, 5, 7, 9];
export function RowsRow({ settings, setSetting, title, description }) {
  const t = useText();
  const r = t.general.rows;
  const value = settings.candidateRows;
  const custom = !standardRows.includes(value);
  const [other, setOther] = useState(custom);
  const [draft, setDraft] = useState(String(custom ? value : 12));
  const [min, max] = candidateRowsRange;
  const commit = () => {
    const rows = Math.round(Number(draft));
    if (rows >= min && rows <= max) setSetting("candidateRows", rows);
    else setDraft(String(custom ? value : 12));
  };
  const choose = (choice) => {
    if (choice === "other") {
      setOther(true);
      commit();
    } else {
      setOther(false);
      setSetting("candidateRows", choice);
    }
  };
  return (
    <Row icon="format_list_numbered" title={title} description={description}
      sub={(other || custom) && (
        <div className="number-line">
          <SpinButton min={min} max={max} aria-label={title} value={Number(draft)}
            onChange={(event, data) => {
              const rows = Math.round(data.value ?? Number(data.displayValue));
              if (!Number.isFinite(rows)) return;
              setDraft(String(rows));
              if (rows >= min && rows <= max) setSetting("candidateRows", rows);
            }} />
          <span>{r.unit}</span>
          <small>{r.range(min, max)}</small>
        </div>
      )}>
      <Segmented label={title} value={other || custom ? "other" : value}
        options={[[0, r.automatic], [5, "5"], [7, "7"], [9, "9"], ["other", r.other]]} onChange={choose} />
    </Row>
  );
}

// Encryption of the user's database, and its recovery code (shown once,
// when made).
export function EncryptionRow({ encryption, runAction, setConfirm }) {
  const t = useText();
  const e = t.dictionary.encryption;
  const [code, setCode] = useState(null);
  const showCode = (response) => { if (response.ok && response.code) setCode(response.code); };
  const turnOn = async () => showCode(await runAction("encryption.enable", {}));
  const turnOff = () => setConfirm({ title: e.offTitle, message: e.offBody, confirmLabel: e.off,
    action: () => runAction("encryption.disable", {}) });
  const renew = () => setConfirm({ title: e.renewTitle, message: e.renewBody, confirmLabel: e.renew,
    action: async () => showCode(await runAction("encryption.renew", {})) });
  const status = encryption.wanted ? (encryption.encrypted ? e.on : e.pendingOn)
    : encryption.encrypted ? e.pendingOff : e.description;
  return (
    <>
      <Row icon="lock" title={e.title} description={status}
          sub={encryption.wanted && (
            <div className="row-sub-action"><Button variant="quiet" onClick={renew}>{e.renew}</Button></div>
          )}>
          <Toggle label={e.title} checked={encryption.wanted} onChange={(value) => (value ? turnOn() : turnOff())} />
        </Row>
      <Dialog open={Boolean(code)} title={e.codeTitle} onClose={() => setCode(null)}>
        <p className="dialog-message">{e.codeBody}</p>
        <p className="recovery-code">{code}</p>
        <div className="dialog-actions">
          <Button onClick={() => runAction("encryption.copy", { code }, e.copied)}>{e.copy}</Button>
          <Button onClick={() => runAction("encryption.save", { code })}>{e.save}</Button>
          <Button variant="primary" onClick={() => setCode(null)}>{e.done}</Button>
        </div>
      </Dialog>
    </>
  );
}

// Mix's own key for each of Japanese mode's ways of writing
// (UserSettings::kMixStyleNumbers, kMixStyleSwitches).
export const mixPairs = {
  japanesePunctuation: "mixPunctuation", japaneseDigitWidth: "mixDigitWidth", japaneseNumberStyle: "mixNumberStyle",
  japaneseKanjiRange: "mixKanjiRange", japaneseKanjiGrade: "mixKanjiGrade", japaneseSymbolWidth: "mixSymbolWidth",
  japaneseLetterWidth: "mixLetterWidth", japaneseSpaceWidth: "mixSpaceWidth", japaneseNotationLevel: "mixNotationLevel",
  japaneseRoughNumbersKanji: "mixRoughNumbersKanji", japaneseKanjiNames: "mixKanjiNames", japaneseKanjiVariants: "mixKanjiVariants",
  japaneseSentenceCheck: "mixSentenceCheck", japaneseSlipAutoApply: "mixSlipAutoApply", japaneseTypedForms: "mixTypedForms",
  japaneseLiveRefine: "mixLiveRefine", japaneseEnglishSpacing: "mixEnglishSpacing",
};

// A row of Japanese mode's ways of writing that Mix also has. Linked, its
// control sets both; set apart, tabs choose whose values it shows and sets.
// `render(settings, setSetting)` draws the row with the values to show.
export function LinkedStyle({ id, keys, settings, setSetting, on, render }) {
  const t = useText();
  const l = t.linked;
  const pairs = keys.filter((key) => mixPairs[key]);
  const both = on.japanese && on.mixed;
  const [unlinked, setUnlinked] = useState(() => readUnlinked(id));
  const [mode, setMode] = useState("japanese");
  const differ = pairs.some((key) => settings[key] !== settings[mixPairs[key]]);
  const apart = both && (unlinked || differ);
  const editing = !both ? (on.japanese ? "japanese" : "mixed") : apart ? mode : "japanese";
  const shown = editing === "mixed" ? { ...settings, ...Object.fromEntries(pairs.map((key) => [key, settings[mixPairs[key]]])) } : settings;
  const set = async (key, value) => {
    const mixKey = mixPairs[key];
    if (!mixKey) return setSetting(key, value);
    if (both && !apart) return (await setSetting(key, value)) && setSetting(mixKey, value);
    return setSetting(editing === "mixed" ? mixKey : key, value);
  };
  const link = async () => {
    writeUnlinked(id, false);
    setUnlinked(false);
    for (const key of pairs) if (settings[key] !== settings[mixPairs[key]]) await setSetting(mixPairs[key], settings[key]);
  };
  const unlink = () => {
    writeUnlinked(id, true);
    setUnlinked(true);
  };
  const tab = (id2) => <span className="mode-tab"><ModeIcon id={id2} size={16} />{t.modeNames[id2]}</span>;
  return (
    <>
      {render(shown, set)}
      {both && (
        <div className="linked-strip">
          {apart && <Tabs size="small" label={l.whose} value={mode} options={[["japanese", tab("japanese")], ["mixed", tab("mixed")]]} onChange={setMode} />}
          <Button variant="quiet" size="small" icon={apart ? "link" : "link_off"} onClick={apart ? link : unlink}>
            {apart ? l.link(t.modeNames.japanese, t.modeNames.mixed) : l.unlink(t.modeNames.japanese, t.modeNames.mixed)}
          </Button>
        </div>
      )}
    </>
  );
}
