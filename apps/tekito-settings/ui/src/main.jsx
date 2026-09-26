import React, { useCallback, useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { CapitataLogo } from "./CapitataLogo.jsx";
import { hostRequest, onHostStateChanged } from "./host.js";
import { Backdrop, Button, Dialog, Glass, Icon, Row, Segmented, Tile, Toggle } from "./ui.jsx";
import "./styles.css";

const pages = [
  { id: "general", label: "General", icon: "general", description: "Choose how TEKITO handles your typing." },
  { id: "typing", label: "Typing", icon: "typing", description: "Choose what Auto mode does for you." },
  { id: "dictionary", label: "Dictionary", icon: "dictionary", description: "Your own words, and what TEKITO learns from your choices." },
  { id: "about", label: "About", icon: "about", description: "Version, language data and support." },
];

// Mirrors the defaults in src/UserData/UserSettings.h; only shown
// until the host's first reply arrives.
const initialSettings = {
  restoreLastInputMode: true,
  correctionEnabled: true,
  commonMisspellingsEnabled: true,
  contextSuggestionsEnabled: true,
  completionEnabled: true,
  candidateWindowEnabled: true,
  learningEnabled: true,
  japanesePhoneticSuggestionsEnabled: true,
  socialExpressionRange: 1,
  socialPersonalization: 1,
  candidateWindowStyle: 0,
  toggleKey: 1,
  periodOnEnter: false,
  excludedApps: [],
  builtInExcludedApps: [],
};

// Only the switches a person would actually want to flip. Common
// misspellings and context ranking stay on (they only make corrections
// better) and are no longer shown.
const features = [
  { key: "correctionEnabled", icon: "spelling", title: "Spelling correction", description: "Fix likely typos when you press Space." },
  { key: "completionEnabled", icon: "completion", title: "Word completion", description: "Offer the rest of a word early." },
  { key: "candidateWindowEnabled", icon: "candidates", title: "Candidate list", description: "Show choices while you type." },
  { key: "japanesePhoneticSuggestionsEnabled", icon: "japanese", title: "Japanese sounds", description: "Suggest English from romaji." },
];

const expressionRanges = [[0, "Off"], [1, "Common"], [2, "Familiar"], [3, "Broad"], [4, "Full"]];

// UserSettings::toggleKey.
const toggleKeys = [[1, "Alt+`"], [2, "Ctrl+Space"], [3, "Ctrl+Shift+Space"], [0, "None"]];

// Candidate list look (UserSettings::candidateWindowStyle).
const candidateStyles = [
  { value: 0, id: "glass", title: "Glass list", description: "Frosted glass, with light under your choice." },
  { value: 1, id: "simple", title: "Simple list", description: "A plain, solid list with a marker." },
];

// The six engine policies boil down to three behaviors a person can tell
// apart: replace on Space, only suggest, or never touch the word.
const actions = {
  replace: { label: "Replace", policy: "Correct spelling", help: "Space changes it for you." },
  suggest: { label: "Suggest", policy: "Suggest alternative", help: "Shown as a choice; never changed on its own." },
  keep: { label: "Keep as typed", policy: "Protect original", help: "TEKITO never corrects this word." },
};

function actionOf(policy) {
  if (policy === "Protect original") return "keep";
  if (policy === "Correct spelling" || policy === "Normalize") return "replace";
  return "suggest";
}

function plural(count, singular, pluralForm = `${singular}s`) {
  return `${count.toLocaleString("en-US")} ${count === 1 ? singular : pluralForm}`;
}

function App() {
  // "#typing" etc. opens a page directly.
  const [page, setPage] = useState(() => pages.find((item) => `#${item.id}` === window.location.hash)?.id || "general");
  const [connection, setConnection] = useState({ status: "loading", error: "" });
  const [settings, setSettings] = useState(initialSettings);
  const [mode, setMode] = useState("auto");
  const [runtime, setRuntime] = useState({ tsf: "Unavailable", dataPacks: "Unavailable" });
  const [learning, setLearning] = useState({ enabled: true, count: 0 });
  const [dictionary, setDictionary] = useState([]);
  const [packs, setPacks] = useState([]);
  const [accent, setAccent] = useState("#0078d4");
  const [modal, setModal] = useState(null);
  const [confirm, setConfirm] = useState(null);
  const [notice, setNotice] = useState(null);

  const applyState = useCallback((state) => {
    if (!state) return;
    setSettings((value) => ({ ...value, ...(state.settings || {}) }));
    setMode(state.mode === "direct" ? "direct" : "auto");
    setRuntime((value) => ({ ...value, ...(state.runtime || {}) }));
    setLearning((value) => ({ ...value, ...(state.learning || {}) }));
    setDictionary(state.dictionary || []);
    setPacks(state.packs || []);
    if (state.appearance?.accent) setAccent(state.appearance.accent);
  }, []);

  const reload = useCallback(async () => {
    const response = await hostRequest("settings.get");
    if (response.ok && response.state) {
      applyState(response.state);
      setConnection({ status: "ready", error: "" });
    } else {
      setConnection({ status: "error", error: response.error || "The settings host could not be reached." });
    }
  }, [applyState]);

  useEffect(() => {
    reload();
    // The host pushes changes made elsewhere (e.g. the Language Bar), so
    // there is no polling here.
    return onHostStateChanged(applyState);
  }, [reload, applyState]);

  useEffect(() => {
    document.documentElement.style.setProperty("--accent", accent);
  }, [accent]);

  const showNotice = (message, tone = "positive") => setNotice({ message, tone, key: Date.now() });
  useEffect(() => {
    if (!notice) return undefined;
    const timer = window.setTimeout(() => setNotice(null), 2400);
    return () => window.clearTimeout(timer);
  }, [notice]);

  const setSetting = async (key, value) => {
    const previous = settings[key];
    setSettings((current) => ({ ...current, [key]: value }));
    if (key === "learningEnabled") setLearning((current) => ({ ...current, enabled: value }));
    const response = await hostRequest("settings.set", { key, value });
    if (response.ok) {
      applyState(response.state);
    } else {
      setSettings((current) => ({ ...current, [key]: previous }));
      if (key === "learningEnabled") setLearning((current) => ({ ...current, enabled: previous }));
      showNotice(response.error || "The setting could not be saved.", "negative");
    }
    return response.ok;
  };

  const changeMode = async (value) => {
    if (value === mode) return;
    const previous = mode;
    setMode(value);
    const response = await hostRequest("mode.set", { value });
    if (response.ok) {
      applyState(response.state);
    } else {
      setMode(previous);
      showNotice(response.error || "The input mode could not be saved.", "negative");
    }
  };

  const runAction = async (type, payload, successMessage) => {
    const response = await hostRequest(type, payload);
    if (response.ok) {
      applyState(response.state);
      if (successMessage) showNotice(successMessage);
    } else {
      showNotice(response.error || "That did not work.", "negative");
    }
    return response;
  };

  const saveDictionary = async (entry, update) => {
    const response = await runAction(update ? "dictionary.update" : "dictionary.create", {
      ...entry,
      ...(update ? { id: modal?.entry?.id } : {}),
    }, update ? "Word updated" : "Word added");
    if (response.ok) setModal(null);
    return response.ok;
  };

  const current = pages.find((item) => item.id === page) || pages[0];

  return (
    <div className="app">
      <Backdrop />
      <Glass as="aside" className="sidebar">
        <div className="brand">
          <img src="./assets/tekito.ico" alt="" />
          <span>
            <img className="wordmark" src="./assets/tekito-wordmark-dark.svg" alt="TEKITO" />
            <small>Settings</small>
          </span>
        </div>
        <nav aria-label="Settings">
          {pages.map((item) => (
            <button key={item.id} type="button" className={`nav-item ${page === item.id ? "is-current" : ""}`}
              aria-current={page === item.id ? "page" : undefined} onClick={() => setPage(item.id)}>
              <Icon name={item.icon} />
              <span>{item.label}</span>
            </button>
          ))}
        </nav>
      </Glass>

      <main className="content">
        <header className="page-header">
          <h1>{current.label}</h1>
          <p>{current.description}</p>
        </header>
        {connection.status !== "ready" ? (
          <Glass className="card state-card" role="status">
            <h2>{connection.status === "loading" ? "Loading settings" : "Settings unavailable"}</h2>
            <p>{connection.status === "loading" ? "Connecting to TEKITO on this PC." : connection.error}</p>
            {connection.status === "error" && <Button variant="primary" onClick={reload}>Try again</Button>}
          </Glass>
        ) : (
          <div className="page" key={page}>
            {page === "general" && <GeneralPage mode={mode} settings={settings} changeMode={changeMode} setSetting={setSetting} runAction={runAction} />}
            {page === "typing" && <TypingPage settings={settings} setSetting={setSetting} />}
            {page === "dictionary" && <DictionaryPage learning={learning} settings={settings} setSetting={setSetting} dictionary={dictionary} setModal={setModal} setConfirm={setConfirm} runAction={runAction} />}
            {page === "about" && <AboutPage runtime={runtime} packs={packs} runAction={runAction} />}
          </div>
        )}
      </main>

      <Dialog open={Boolean(modal)} title={modal?.mode === "edit" ? "Edit word" : "Add a word"} onClose={() => setModal(null)}>
        <DictionaryForm entry={modal?.entry || {}} onCancel={() => setModal(null)} onSave={(entry) => saveDictionary(entry, modal?.mode === "edit")} />
      </Dialog>
      <Dialog open={Boolean(confirm)} title={confirm?.title} onClose={() => setConfirm(null)}>
        <p className="dialog-message">{confirm?.message}</p>
        <div className="dialog-actions">
          <Button onClick={() => setConfirm(null)}>Cancel</Button>
          <Button variant="danger-solid" onClick={async () => { const action = confirm.action; setConfirm(null); await action(); }}>{confirm?.confirmLabel}</Button>
        </div>
      </Dialog>
      {notice && <div key={notice.key} className={`toast glass ${notice.tone}`} role="status" aria-live="polite">{notice.tone === "positive" && <span className="dot" />}{notice.message}</div>}
    </div>
  );
}

function GeneralPage({ mode, settings, changeMode, setSetting, runAction }) {
  return (
    <>
      <div className="tile-grid mode-grid">
        <Tile on={mode === "auto"} image="./assets/auto.ico" title="Auto" status={mode === "auto" ? "In use" : undefined}
          description="Corrects spelling and suggests words as you type." onPress={() => changeMode("auto")} />
        <Tile on={mode === "direct"} image="./assets/direct.ico" title="Direct" status={mode === "direct" ? "In use" : undefined}
          description="Types exactly the keys you press." onPress={() => changeMode("direct")} />
      </div>
      <Glass className="card">
        <Row title="Start in the last mode used" description="Otherwise TEKITO always starts in Auto.">
          <Toggle label="Start in the last mode used" checked={settings.restoreLastInputMode} onChange={(value) => setSetting("restoreLastInputMode", value)} />
        </Row>
        <Row title="Switch key" description="Toggles Auto and Direct from any app.">
          <Segmented label="Switch key" value={settings.toggleKey} options={toggleKeys} onChange={(value) => setSetting("toggleKey", value)} />
        </Row>
      </Glass>
      <ExcludedApps settings={settings} runAction={runAction} />
    </>
  );
}

function ExcludedApps({ settings, runAction }) {
  const [name, setName] = useState("");
  const add = async (event) => {
    event.preventDefault();
    if (!name.trim()) return;
    const response = await runAction("excludedApps.add", { name: name.trim() });
    if (response.ok) setName("");
  };
  return (
    <Glass className="card">
      <div className="card-head">
        <div>
          <h2 className="card-title">Apps where TEKITO stays off</h2>
          <p className="muted">Every key goes straight through, as if TEKITO were not installed.</p>
        </div>
        <Button icon="folder" onClick={() => runAction("excludedApps.browse", {})}>Choose app</Button>
      </div>
      <ul className="app-list" aria-label="Apps where TEKITO stays off">
        {settings.excludedApps.map((app) => (
          <li key={app} className="word">
            <span className="app-name">{app}</span>
            <button type="button" className="icon-btn" aria-label={`Turn TEKITO back on in ${app}`}
              onClick={() => runAction("excludedApps.remove", { name: app })}>
              <Icon name="trash" size={16} />
            </button>
          </li>
        ))}
      </ul>
      <form className="app-add" onSubmit={add}>
        <label className="search">
          <Icon name="plus" size={17} />
          <input aria-label="App name" placeholder="App name, such as code.exe" value={name} onChange={(event) => setName(event.target.value)} />
        </label>
        <Button type="submit" disabled={!name.trim()}>Add</Button>
      </form>
      {settings.builtInExcludedApps.length > 0 && (
        <p className="fine-print">Always off in terminals: {settings.builtInExcludedApps.join(", ")}.</p>
      )}
    </Glass>
  );
}

function TypingPage({ settings, setSetting }) {
  return (
    <>
      <div className="tile-grid feature-grid">
        {features.map((feature) => (
          <Tile key={feature.key} on={Boolean(settings[feature.key])} icon={feature.icon} title={feature.title}
            description={feature.description} status={settings[feature.key] ? "On" : "Off"}
            onPress={() => setSetting(feature.key, !settings[feature.key])} />
        ))}
      </div>
      <div className="tile-grid mode-grid">
        {candidateStyles.map((style) => (
          <Tile key={style.value} on={settings.candidateWindowStyle === style.value} title={style.title}
            description={style.description} status={settings.candidateWindowStyle === style.value ? "In use" : undefined}
            art={<MiniList variant={style.id} />} onPress={() => setSetting("candidateWindowStyle", style.value)} />
        ))}
      </div>
      <Glass className="card">
        <Row title="Casual expressions" description="How much chat vocabulary, like “gonna” or “lol”, shows up.">
          <Segmented label="Casual expressions" value={settings.socialExpressionRange} options={expressionRanges} onChange={(value) => setSetting("socialExpressionRange", value)} />
        </Row>
        <Row title="Period at the end of a line" description="Enter adds a period when a line ends without punctuation.">
          <Toggle label="Period at the end of a line" checked={settings.periodOnEnter} onChange={(value) => setSetting("periodOnEnter", value)} />
        </Row>
      </Glass>
      <Glass className="card">
        <h2 className="card-title">Keys</h2>
        <div className="keys">
          <Key label="Correct, then next choice" keys={["Space"]} />
          <Key label="Previous choice" keys={["Shift", "Space"]} />
          <Key label="Browse choices" keys={["Tab", "↑", "↓"]} />
          <Key label="Undo the correction" keys={["Backspace"]} />
          <Key label="Keep what you typed" keys={["Esc"]} />
        </div>
      </Glass>
    </>
  );
}

// A tiny sketch of each candidate list style for the style picker.
function MiniList({ variant }) {
  return (
    <span className={`mini-list ${variant}`} aria-hidden="true">
      <i /><i className="is-selected" /><i />
    </span>
  );
}

function Key({ label, keys }) {
  return <div className="key"><span>{keys.map((key) => <kbd key={key}>{key}</kbd>)}</span><small>{label}</small></div>;
}

function DictionaryPage({ learning, settings, setSetting, dictionary, setModal, setConfirm, runAction }) {
  const [query, setQuery] = useState("");
  const filtered = useMemo(() => {
    const needle = query.trim().toLowerCase();
    return dictionary.filter((entry) => !needle || `${entry.raw} ${entry.candidate}`.toLowerCase().includes(needle));
  }, [dictionary, query]);

  // One switch for everything TEKITO learns from you: which corrections you
  // pick and which casual expressions you favor.
  const setLearningEnabled = async (value) => {
    if (await setSetting("learningEnabled", value)) {
      if (Boolean(settings.socialPersonalization) !== value) await setSetting("socialPersonalization", value ? 1 : 0);
    }
  };
  const askDelete = (entry) => setConfirm({
    title: "Remove this word?",
    message: `“${entry.raw}” will be removed from your dictionary.`,
    confirmLabel: "Remove",
    action: () => runAction("dictionary.delete", { id: entry.id }, "Word removed"),
  });
  const askClear = () => learning.count && setConfirm({
    title: "Forget what TEKITO learned?",
    message: "Your choices so far will no longer affect suggestions. This cannot be undone.",
    confirmLabel: "Forget",
    action: () => runAction("learning.clear", {}, "Learning data cleared"),
  });

  return (
    <>
      <Glass className="card">
        <Row title="Learn from my choices" description="Words and expressions you pick move up next time.">
          <Toggle label="Learn from my choices" checked={learning.enabled} onChange={setLearningEnabled} />
        </Row>
        <div className="learning-meta">
          <span className="muted">{learning.count ? `Learned from ${plural(learning.count, "choice")} so far.` : "Nothing learned yet."}</span>
          <Button variant="quiet" disabled={!learning.count} onClick={askClear}>Forget</Button>
        </div>
      </Glass>

      <Glass className="card">
        <div className="card-head">
          <div>
            <h2 className="card-title">Your words</h2>
            <p className="muted">Names, abbreviations and anything TEKITO should handle your way.</p>
          </div>
          <Button variant="primary" icon="plus" onClick={() => setModal({ mode: "new", entry: {} })}>Add</Button>
        </div>
        {dictionary.length > 0 && (
          <label className="search">
            <Icon name="search" size={17} />
            <input aria-label="Search your words" placeholder="Search" value={query} onChange={(event) => setQuery(event.target.value)} />
          </label>
        )}
        <ul className="word-list" aria-label="Your words">
          {filtered.map((entry) => {
            const action = actionOf(entry.policy);
            return (
              <li key={entry.id} className="word">
                <button type="button" className="word-main" onClick={() => setModal({ mode: "edit", entry })}>
                  <b>{entry.raw}</b>
                  {action !== "keep" && <><Icon name="arrow" size={15} className="word-arrow" /><span>{entry.candidate}</span></>}
                  <span className={`badge ${action}`}>{actions[action].label}</span>
                </button>
                <button type="button" className="icon-btn" aria-label={`Remove ${entry.raw}`} onClick={() => askDelete(entry)}>
                  <Icon name="trash" size={16} />
                </button>
              </li>
            );
          })}
        </ul>
        {filtered.length === 0 && (
          <div className="empty">{dictionary.length === 0 ? "No words yet. Add a name or abbreviation you use often." : "Nothing matches your search."}</div>
        )}
        <div className="card-foot">
          <Button variant="quiet" icon="import" onClick={() => runAction("dictionary.import", {}, "Words imported")}>Import</Button>
          <Button variant="quiet" icon="export" onClick={() => runAction("dictionary.export", {}, "Words exported")}>Export</Button>
        </div>
      </Glass>
    </>
  );
}

function AboutPage({ runtime, packs, runAction }) {
  const [license, setLicense] = useState(null);
  const openLicense = async () => {
    const response = await hostRequest("license.get");
    setLicense(response.ok ? response.text : "The license text could not be loaded.");
  };
  const problems = packs.filter((pack) => !pack.valid);
  const installed = runtime.tsf === "Loaded";
  return (
    <>
      <Glass className="card about-hero">
        <img className="about-icon" src="./assets/tekito.ico" alt="" />
        <div>
          <img className="wordmark large" src="./assets/tekito-wordmark-dark.svg" alt="TEKITO" />
          <p>Easy English typing for Windows.</p>
        </div>
        <span className="version"><small>Version</small><b>0.1.0</b></span>
      </Glass>

      <Glass className="card">
        <div className="status-line">
          <span><b>Input method</b><small>{installed ? "Installed and registered with Windows." : "Not registered with Windows. Reinstall TEKITO."}</small></span>
          <span className={`state ${installed ? "" : "problem"}`}><span className="dot" />{installed ? "Ready" : "Problem"}</span>
        </div>
        <div className="status-line">
          <span><b>Language data</b><small>{problems.length === 0 ? `All ${packs.length} packs are installed and verified.` : `${runtime.dataPacks} ready. Some suggestions may be missing.`}</small></span>
          <span className={`state ${problems.length ? "problem" : ""}`}><span className="dot" />{problems.length ? "Problem" : "Ready"}</span>
        </div>
        {problems.map((pack) => (
          <div className="pack-problem" key={pack.packPath}><b>{pack.displayName}</b><span>{pack.reason || "Unavailable"}</span></div>
        ))}
        <div className="card-foot">
          <Button variant="quiet" icon="copy" onClick={() => runAction("diagnostics.copy", {}, "Diagnostics copied")}>Copy diagnostics</Button>
          <Button variant="quiet" icon="folder" onClick={() => runAction("logs.open", {})}>Log folder</Button>
          <Button variant="quiet" icon="license" onClick={openLicense}>License</Button>
          <Button variant="quiet" icon="folder" onClick={() => runAction("notices.open", {})}>Third-party notices</Button>
        </div>
        <p className="fine-print">TEKITO works entirely on this PC. What you type is never sent anywhere, and diagnostics never include it.</p>
      </Glass>

      <Glass className="card publisher">
        <span className="publisher-label">Distributed by</span>
        <CapitataLogo className="capitata" />
        <Button variant="quiet" icon="globe" onClick={() => runAction("website.open", {})}>capitata.dev</Button>
      </Glass>

      <Dialog open={license !== null} title="TEKITO License Agreement" wide onClose={() => setLicense(null)}>
        <LicenseText text={license || ""} />
        <div className="dialog-actions"><Button variant="primary" onClick={() => setLicense(null)}>Close</Button></div>
      </Dialog>
    </>
  );
}

// Renders the license Markdown's few constructs: headings, bullets, paragraphs.
function LicenseText({ text }) {
  const blocks = [];
  text.replace(/\r/g, "").split("\n").forEach((raw, index) => {
    const line = raw.trim();
    const last = blocks[blocks.length - 1];
    if (line.startsWith("- ")) {
      if (last?.kind === "list") last.items.push(line.slice(2));
      else blocks.push({ kind: "list", items: [line.slice(2)] });
    } else if (line.startsWith("## ")) {
      blocks.push({ kind: "heading", text: line.slice(3) });
    } else if (line && index > 0) {  // line 0 is the title, shown as the dialog heading
      blocks.push({ kind: "paragraph", text: line });
    }
  });
  return (
    <div className="license-text" tabIndex={0}>
      {blocks.map((block, index) => block.kind === "list"
        ? <ul key={index}>{block.items.map((item) => <li key={item}>{item}</li>)}</ul>
        : block.kind === "heading" ? <h3 key={index}>{block.text}</h3> : <p key={index}>{block.text}</p>)}
    </div>
  );
}

function DictionaryForm({ entry, onSave, onCancel }) {
  const [value, setValue] = useState({ raw: entry.raw || "", candidate: entry.candidate || "", action: entry.policy ? actionOf(entry.policy) : "replace" });
  const keep = value.action === "keep";
  const valid = value.raw.trim() && (keep || value.candidate.trim());
  const submit = (event) => {
    event.preventDefault();
    if (!valid) return;
    const raw = value.raw.trim();
    onSave({ raw, candidate: keep ? raw : value.candidate.trim(), policy: actions[value.action].policy });
  };
  return (
    <form className="form" onSubmit={submit}>
      <label className="field"><span>When I type</span><input autoFocus value={value.raw} placeholder="e.g. omw" onChange={(event) => setValue({ ...value, raw: event.target.value })} /></label>
      <div className="field">
        <span>TEKITO should</span>
        <Segmented label="What TEKITO should do" value={value.action} options={Object.entries(actions).map(([key, item]) => [key, item.label])}
          onChange={(action) => setValue({ ...value, action })} />
        <small className="field-help">{actions[value.action].help}</small>
      </div>
      {!keep && <label className="field"><span>Change it to</span><input value={value.candidate} placeholder="e.g. on my way" onChange={(event) => setValue({ ...value, candidate: event.target.value })} /></label>}
      <div className="dialog-actions">
        <Button onClick={onCancel}>Cancel</Button>
        <Button type="submit" variant="primary" disabled={!valid}>Save</Button>
      </div>
    </form>
  );
}

const start = import.meta.env.DEV && !window.chrome?.webview
  ? import("./devHost.js").then((module) => module.installMockHost())
  : Promise.resolve();
start.then(() => createRoot(document.getElementById("root")).render(<App />));
