import React, { useCallback, useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { CapitataLogo } from "./CapitataLogo.jsx";
import { hostRequest, onHostStateChanged } from "./host.js";
import { messages, resolveLanguage, TextContext, useText } from "./i18n.js";
import { Backdrop, Button, Dialog, Glass, Icon, Row, Segmented, Tile, Toggle } from "./ui.jsx";
import "./styles.css";

const pages = [
  { id: "general", icon: "general" },
  { id: "typing", icon: "typing" },
  { id: "japanese", icon: "japanese" },
  { id: "dictionary", icon: "dictionary" },
  { id: "about", icon: "about" },
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
  keyboardType: 0,
  periodOnEnter: false,
  japaneseSpaceWidth: 0,
  japanesePunctuation: 0,
  japanesePredictionEnabled: true,
  japaneseDropSpaceBeforeKana: true,
  uiLanguage: 0,
  excludedApps: [],
  builtInExcludedApps: [],
};

// Only the switches a person would actually want to flip. Common
// misspellings and context ranking stay on (they only make corrections
// better) and are no longer shown.
const features = [
  { key: "correctionEnabled", icon: "spelling" },
  { key: "completionEnabled", icon: "completion" },
  { key: "candidateWindowEnabled", icon: "candidates" },
  { key: "japanesePhoneticSuggestionsEnabled", icon: "japanese" },
];

// Candidate list look (UserSettings::candidateWindowStyle).
const candidateStyles = [{ value: 0, id: "glass" }, { value: 1, id: "simple" }];

// The six engine policies boil down to three behaviors a person can tell
// apart: replace on Space, only suggest, or never touch the word.
const actionPolicies = { replace: "Correct spelling", suggest: "Suggest alternative", keep: "Protect original" };

function actionOf(policy) {
  if (policy === "Protect original") return "keep";
  if (policy === "Correct spelling" || policy === "Normalize") return "replace";
  return "suggest";
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
  const [version, setVersion] = useState("");
  const [appearance, setAppearance] = useState({ accent: "#0078d4", systemLanguage: navigator.language.startsWith("ja") ? "ja" : "en" });
  const [modal, setModal] = useState(null);
  const [confirm, setConfirm] = useState(null);
  const [notice, setNotice] = useState(null);

  const t = messages[resolveLanguage(settings.uiLanguage, appearance.systemLanguage)];

  const applyState = useCallback((state) => {
    if (!state) return;
    setSettings((value) => ({ ...value, ...(state.settings || {}) }));
    setMode(state.mode === "direct" ? "direct" : "auto");
    setRuntime((value) => ({ ...value, ...(state.runtime || {}) }));
    setLearning((value) => ({ ...value, ...(state.learning || {}) }));
    setDictionary(state.dictionary || []);
    setPacks(state.packs || []);
    if (state.version) setVersion(state.version);
    if (state.appearance) setAppearance((value) => ({ ...value, ...state.appearance }));
  }, []);

  const reload = useCallback(async () => {
    const response = await hostRequest("settings.get");
    if (response.ok && response.state) {
      applyState(response.state);
      setConnection({ status: "ready", error: "" });
    } else {
      setConnection({ status: "error", error: response.error });
    }
  }, [applyState]);

  useEffect(() => {
    reload();
    // The host pushes changes made elsewhere (e.g. the Language Bar), so
    // there is no polling here.
    return onHostStateChanged(applyState);
  }, [reload, applyState]);

  useEffect(() => {
    document.documentElement.style.setProperty("--accent", appearance.accent);
  }, [appearance.accent]);

  useEffect(() => {
    document.documentElement.lang = t.lang;
    document.title = `TEKITO ${t.settings}`;
  }, [t]);

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
      showNotice(response.error || t.settingNotSaved, "negative");
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
      showNotice(response.error || t.modeNotSaved, "negative");
    }
  };

  const runAction = async (type, payload, successMessage) => {
    const response = await hostRequest(type, payload);
    if (response.ok) {
      applyState(response.state);
      if (successMessage) showNotice(successMessage);
    } else {
      showNotice(response.error || t.failed, "negative");
    }
    return response;
  };

  const saveDictionary = async (entry, update) => {
    const response = await runAction(update ? "dictionary.update" : "dictionary.create", {
      ...entry,
      ...(update ? { id: modal?.entry?.id } : {}),
    }, update ? t.words.updated : t.words.added);
    if (response.ok) setModal(null);
    return response.ok;
  };

  const current = pages.find((item) => item.id === page) || pages[0];

  return (
    <TextContext.Provider value={t}>
      <div className="app">
        <Backdrop />
        <Glass as="aside" className="sidebar">
          <div className="brand">
            <img src="./assets/tekito.ico" alt="" />
            <span>
              <img className="wordmark" src="./assets/tekito-wordmark-dark.svg" alt="TEKITO" />
              <small>{t.settings}</small>
            </span>
          </div>
          <nav aria-label={t.settings}>
            {pages.map((item) => (
              <button key={item.id} type="button" className={`nav-item ${page === item.id ? "is-current" : ""}`}
                aria-current={page === item.id ? "page" : undefined} onClick={() => setPage(item.id)}>
                <Icon name={item.icon} />
                <span>{t.pages[item.id].label}</span>
              </button>
            ))}
          </nav>
        </Glass>

        <main className="content">
          <header className="page-header">
            <h1>{t.pages[current.id].label}</h1>
            <p>{t.pages[current.id].description}</p>
          </header>
          {connection.status !== "ready" ? (
            <Glass className="card state-card" role="status">
              <h2>{connection.status === "loading" ? t.loading.title : t.unavailable.title}</h2>
              <p>{connection.status === "loading" ? t.loading.body : connection.error || t.unavailable.body}</p>
              {connection.status === "error" && <Button variant="primary" onClick={reload}>{t.tryAgain}</Button>}
            </Glass>
          ) : (
            <div className="page" key={page}>
              {page === "general" && <GeneralPage mode={mode} settings={settings} changeMode={changeMode} setSetting={setSetting} runAction={runAction} />}
              {page === "typing" && <TypingPage settings={settings} setSetting={setSetting} />}
              {page === "japanese" && <JapanesePage settings={settings} setSetting={setSetting} setConfirm={setConfirm} runAction={runAction} />}
              {page === "dictionary" && <DictionaryPage learning={learning} settings={settings} setSetting={setSetting} dictionary={dictionary} setModal={setModal} setConfirm={setConfirm} runAction={runAction} />}
              {page === "about" && <AboutPage version={version} runtime={runtime} packs={packs} runAction={runAction} />}
            </div>
          )}
        </main>

        <Dialog open={Boolean(modal)} title={modal?.mode === "edit" ? t.words.editTitle : t.words.addTitle} onClose={() => setModal(null)}>
          <DictionaryForm entry={modal?.entry || {}} onCancel={() => setModal(null)} onSave={(entry) => saveDictionary(entry, modal?.mode === "edit")} />
        </Dialog>
        <Dialog open={Boolean(confirm)} title={confirm?.title} onClose={() => setConfirm(null)}>
          <p className="dialog-message">{confirm?.message}</p>
          <div className="dialog-actions">
            <Button onClick={() => setConfirm(null)}>{t.cancel}</Button>
            <Button variant="danger-solid" onClick={async () => { const action = confirm.action; setConfirm(null); await action(); }}>{confirm?.confirmLabel}</Button>
          </div>
        </Dialog>
        {notice && <div key={notice.key} className={`toast glass ${notice.tone}`} role="status" aria-live="polite">{notice.tone === "positive" && <span className="dot" />}{notice.message}</div>}
      </div>
    </TextContext.Provider>
  );
}

function GeneralPage({ mode, settings, changeMode, setSetting, runAction }) {
  const t = useText();
  // UserSettings::toggleKey and UserSettings::uiLanguage.
  const toggleKeys = [[1, "Alt+`"], [2, "Ctrl+Space"], [3, "Ctrl+Shift+Space"], [0, t.switchKey.none]];
  const languages = [[0, t.language.system], [1, "English"], [2, "日本語"]];
  // UserSettings::keyboardType.
  const keyboards = [[0, t.keyboard.detect], [1, t.keyboard.japanese], [2, t.keyboard.us]];
  return (
    <>
      <div className="tile-grid mode-grid">
        <Tile on={mode === "auto"} image="./assets/auto.ico" title="Auto" status={mode === "auto" ? t.inUse : undefined}
          description={t.modes.auto} onPress={() => changeMode("auto")} />
        <Tile on={mode === "direct"} image="./assets/direct.ico" title="Direct" status={mode === "direct" ? t.inUse : undefined}
          description={t.modes.direct} onPress={() => changeMode("direct")} />
      </div>
      <Glass className="card">
        <Row title={t.restoreMode.title} description={t.restoreMode.description}>
          <Toggle label={t.restoreMode.title} checked={settings.restoreLastInputMode} onChange={(value) => setSetting("restoreLastInputMode", value)} />
        </Row>
        <Row title={t.switchKey.title} description={t.switchKey.description}>
          <Segmented label={t.switchKey.title} value={settings.toggleKey} options={toggleKeys} onChange={(value) => setSetting("toggleKey", value)} />
        </Row>
        <Row title={t.keyboard.title} description={t.keyboard.description}>
          <Segmented label={t.keyboard.title} value={settings.keyboardType} options={keyboards} onChange={(value) => setSetting("keyboardType", value)} />
        </Row>
        <Row title={t.language.title} description={t.language.description}>
          <Segmented label={t.language.title} value={settings.uiLanguage} options={languages} onChange={(value) => setSetting("uiLanguage", value)} />
        </Row>
      </Glass>
      <ExcludedApps settings={settings} runAction={runAction} />
    </>
  );
}

function ExcludedApps({ settings, runAction }) {
  const t = useText();
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
          <h2 className="card-title">{t.excluded.title}</h2>
          <p className="muted">{t.excluded.description}</p>
        </div>
        <Button icon="folder" onClick={() => runAction("excludedApps.browse", {})}>{t.excluded.choose}</Button>
      </div>
      <ul className="app-list" aria-label={t.excluded.title}>
        {settings.excludedApps.map((app) => (
          <li key={app} className="word">
            <span className="app-name">{app}</span>
            <button type="button" className="icon-btn" aria-label={t.excluded.remove(app)}
              onClick={() => runAction("excludedApps.remove", { name: app })}>
              <Icon name="trash" size={16} />
            </button>
          </li>
        ))}
      </ul>
      <form className="app-add" onSubmit={add}>
        <label className="search">
          <Icon name="plus" size={17} />
          <input aria-label={t.excluded.name} placeholder={t.excluded.placeholder} value={name} onChange={(event) => setName(event.target.value)} />
        </label>
        <Button type="submit" disabled={!name.trim()}>{t.add}</Button>
      </form>
      {settings.builtInExcludedApps.length > 0 && (
        <p className="fine-print">{t.excluded.builtIn(settings.builtInExcludedApps.join(", "))}</p>
      )}
    </Glass>
  );
}

function JapanesePage({ settings, setSetting, setConfirm, runAction }) {
  const t = useText();
  const j = t.japanese;
  // UserSettings::japaneseSpaceWidth and ::japanesePunctuation.
  const spaces = [[0, j.space.follow], [1, j.space.half], [2, j.space.full]];
  const marks = [[0, "、。"], [1, "，．"], [2, "，。"], [3, "、．"]];
  const askForget = () => setConfirm({
    title: j.forget.confirmTitle,
    message: j.forget.confirmMessage,
    confirmLabel: t.learning.forget,
    action: () => runAction("japaneseLearning.clear", {}, t.learning.cleared),
  });
  return (
    <>
      <Glass className="card">
        <Row title={j.space.title} description={j.space.description}>
          <Segmented label={j.space.title} value={settings.japaneseSpaceWidth} options={spaces} onChange={(value) => setSetting("japaneseSpaceWidth", value)} />
        </Row>
        <Row title={j.punctuation.title} description={j.punctuation.description}>
          <Segmented label={j.punctuation.title} value={settings.japanesePunctuation} options={marks} onChange={(value) => setSetting("japanesePunctuation", value)} />
        </Row>
        <Row title={j.prediction.title} description={j.prediction.description}>
          <Toggle label={j.prediction.title} checked={settings.japanesePredictionEnabled} onChange={(value) => setSetting("japanesePredictionEnabled", value)} />
        </Row>
        <Row title={j.dropSpace.title} description={j.dropSpace.description}>
          <Toggle label={j.dropSpace.title} checked={settings.japaneseDropSpaceBeforeKana} onChange={(value) => setSetting("japaneseDropSpaceBeforeKana", value)} />
        </Row>
        <Row title={j.forget.title} description={j.forget.description}>
          <Button variant="quiet" onClick={askForget}>{t.learning.forget}</Button>
        </Row>
      </Glass>
      <Glass className="card">
        <h2 className="card-title">{t.keys.title}</h2>
        <div className="keys">
          <Key label={j.keys.convert} keys={["Space"]} />
          <Key label={j.keys.phrases} keys={["←", "→"]} />
          <Key label={j.keys.resize} keys={["Shift", "←", "→"]} />
          <Key label={j.keys.predictions} keys={["Tab", "↓"]} />
          <Key label={j.keys.kana} keys={["F6", "F7", "F8"]} />
          <Key label={j.keys.letters} keys={["F9", "F10"]} />
          <Key label={j.keys.english} keys={["Shift", "A-Z"]} />
          <Key label={j.keys.back} keys={["Esc"]} />
        </div>
      </Glass>
    </>
  );
}

function TypingPage({ settings, setSetting }) {
  const t = useText();
  const expressionRanges = t.casual.ranges.map((label, value) => [value, label]);
  return (
    <>
      <div className="tile-grid feature-grid">
        {features.map((feature) => (
          <Tile key={feature.key} on={Boolean(settings[feature.key])} icon={feature.icon} title={t.features[feature.key].title}
            description={t.features[feature.key].description} status={settings[feature.key] ? t.on : t.off}
            onPress={() => setSetting(feature.key, !settings[feature.key])} />
        ))}
      </div>
      <div className="tile-grid mode-grid">
        {candidateStyles.map((style) => (
          <Tile key={style.value} on={settings.candidateWindowStyle === style.value} title={t.styles[style.id].title}
            description={t.styles[style.id].description} status={settings.candidateWindowStyle === style.value ? t.inUse : undefined}
            art={<MiniList variant={style.id} />} onPress={() => setSetting("candidateWindowStyle", style.value)} />
        ))}
      </div>
      <Glass className="card">
        <Row title={t.casual.title} description={t.casual.description}>
          <Segmented label={t.casual.title} value={settings.socialExpressionRange} options={expressionRanges} onChange={(value) => setSetting("socialExpressionRange", value)} />
        </Row>
        <Row title={t.period.title} description={t.period.description}>
          <Toggle label={t.period.title} checked={settings.periodOnEnter} onChange={(value) => setSetting("periodOnEnter", value)} />
        </Row>
      </Glass>
      <Glass className="card">
        <h2 className="card-title">{t.keys.title}</h2>
        <div className="keys">
          <Key label={t.keys.next} keys={["Space"]} />
          <Key label={t.keys.previous} keys={["Shift", "Space"]} />
          <Key label={t.keys.browse} keys={["Tab", "↑", "↓"]} />
          <Key label={t.keys.undo} keys={["Backspace"]} />
          <Key label={t.keys.keep} keys={["Esc"]} />
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
  const t = useText();
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
    title: t.words.removeTitle,
    message: t.words.removeMessage(entry.raw),
    confirmLabel: t.words.remove,
    action: () => runAction("dictionary.delete", { id: entry.id }, t.words.removed),
  });
  const askClear = () => learning.count && setConfirm({
    title: t.learning.confirmTitle,
    message: t.learning.confirmMessage,
    confirmLabel: t.learning.forget,
    action: () => runAction("learning.clear", {}, t.learning.cleared),
  });

  return (
    <>
      <Glass className="card">
        <Row title={t.learning.title} description={t.learning.description}>
          <Toggle label={t.learning.title} checked={learning.enabled} onChange={setLearningEnabled} />
        </Row>
        <div className="learning-meta">
          <span className="muted">{learning.count ? t.learning.learned(learning.count) : t.learning.nothing}</span>
          <Button variant="quiet" disabled={!learning.count} onClick={askClear}>{t.learning.forget}</Button>
        </div>
      </Glass>

      <Glass className="card">
        <div className="card-head">
          <div>
            <h2 className="card-title">{t.words.title}</h2>
            <p className="muted">{t.words.description}</p>
          </div>
          <Button variant="primary" icon="plus" onClick={() => setModal({ mode: "new", entry: {} })}>{t.add}</Button>
        </div>
        {dictionary.length > 0 && (
          <label className="search">
            <Icon name="search" size={17} />
            <input aria-label={t.words.searchLabel} placeholder={t.words.search} value={query} onChange={(event) => setQuery(event.target.value)} />
          </label>
        )}
        <ul className="word-list" aria-label={t.words.title}>
          {filtered.map((entry) => {
            const action = actionOf(entry.policy);
            return (
              <li key={entry.id} className="word">
                <button type="button" className="word-main" onClick={() => setModal({ mode: "edit", entry })}>
                  <b>{entry.raw}</b>
                  {action !== "keep" && <><Icon name="arrow" size={15} className="word-arrow" /><span>{entry.candidate}</span></>}
                  <span className={`badge ${action}`}>{t.actions[action].label}</span>
                </button>
                <button type="button" className="icon-btn" aria-label={t.words.removeLabel(entry.raw)} onClick={() => askDelete(entry)}>
                  <Icon name="trash" size={16} />
                </button>
              </li>
            );
          })}
        </ul>
        {filtered.length === 0 && (
          <div className="empty">{dictionary.length === 0 ? t.words.empty : t.words.noMatch}</div>
        )}
        <div className="card-foot">
          <Button variant="quiet" icon="import" onClick={() => runAction("dictionary.import", {}, t.words.imported)}>{t.words.import}</Button>
          <Button variant="quiet" icon="export" onClick={() => runAction("dictionary.export", {}, t.words.exported)}>{t.words.export}</Button>
        </div>
      </Glass>
    </>
  );
}

function AboutPage({ version, runtime, packs, runAction }) {
  const t = useText();
  const [license, setLicense] = useState(null);
  const openLicense = async () => {
    const response = await hostRequest("license.get");
    setLicense(response.ok ? response.text : t.about.licenseMissing);
  };
  const problems = packs.filter((pack) => !pack.valid);
  const installed = runtime.tsf === "Loaded";
  return (
    <>
      <Glass className="card about-hero">
        <img className="about-icon" src="./assets/tekito.ico" alt="" />
        <div>
          <img className="wordmark large" src="./assets/tekito-wordmark-dark.svg" alt="TEKITO" />
          <p>{t.about.tagline}</p>
        </div>
        <span className="version"><small>{t.about.version}</small><b>{version}</b></span>
      </Glass>

      <Glass className="card">
        <div className="status-line">
          <span><b>{t.about.inputMethod}</b><small>{installed ? t.about.installed : t.about.notInstalled}</small></span>
          <span className={`state ${installed ? "" : "problem"}`}><span className="dot" />{installed ? t.about.ready : t.about.problem}</span>
        </div>
        <div className="status-line">
          <span><b>{t.about.languageData}</b><small>{problems.length === 0 ? t.about.allPacks(packs.length) : t.about.somePacks(runtime.dataPacks)}</small></span>
          <span className={`state ${problems.length ? "problem" : ""}`}><span className="dot" />{problems.length ? t.about.problem : t.about.ready}</span>
        </div>
        {problems.map((pack) => (
          <div className="pack-problem" key={pack.packPath}><b>{pack.displayName}</b><span>{pack.reason || t.about.unavailable}</span></div>
        ))}
        <div className="card-foot">
          <Button variant="quiet" icon="copy" onClick={() => runAction("diagnostics.copy", {}, t.about.diagnosticsCopied)}>{t.about.copyDiagnostics}</Button>
          <Button variant="quiet" icon="folder" onClick={() => runAction("logs.open", {})}>{t.about.logFolder}</Button>
          <Button variant="quiet" icon="license" onClick={openLicense}>{t.about.license}</Button>
          <Button variant="quiet" icon="folder" onClick={() => runAction("notices.open", {})}>{t.about.notices}</Button>
        </div>
        <p className="fine-print">{t.about.privacy}</p>
      </Glass>

      <Glass className="card publisher">
        <span className="publisher-label">{t.about.distributedBy}</span>
        <CapitataLogo className="capitata" />
        <Button variant="quiet" icon="globe" onClick={() => runAction("website.open", {})}>capitata.dev</Button>
      </Glass>

      <Dialog open={license !== null} title={t.about.licenseTitle} wide onClose={() => setLicense(null)}>
        {t.about.licenseNote && <p className="muted">{t.about.licenseNote}</p>}
        <LicenseText text={license || ""} />
        <div className="dialog-actions"><Button variant="primary" onClick={() => setLicense(null)}>{t.close}</Button></div>
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
    <div className="license-text" tabIndex={0} lang="en">
      {blocks.map((block, index) => block.kind === "list"
        ? <ul key={index}>{block.items.map((item) => <li key={item}>{item}</li>)}</ul>
        : block.kind === "heading" ? <h3 key={index}>{block.text}</h3> : <p key={index}>{block.text}</p>)}
    </div>
  );
}

function DictionaryForm({ entry, onSave, onCancel }) {
  const t = useText();
  const [value, setValue] = useState({ raw: entry.raw || "", candidate: entry.candidate || "", action: entry.policy ? actionOf(entry.policy) : "replace" });
  const keep = value.action === "keep";
  const valid = value.raw.trim() && (keep || value.candidate.trim());
  const submit = (event) => {
    event.preventDefault();
    if (!valid) return;
    const raw = value.raw.trim();
    onSave({ raw, candidate: keep ? raw : value.candidate.trim(), policy: actionPolicies[value.action] });
  };
  return (
    <form className="form" onSubmit={submit}>
      <label className="field"><span>{t.words.whenITypeLabel}</span><input autoFocus value={value.raw} placeholder={t.words.whenITypePlaceholder} onChange={(event) => setValue({ ...value, raw: event.target.value })} /></label>
      <div className="field">
        <span>{t.words.actionLabel}</span>
        <Segmented label={t.words.actionLabel} value={value.action} options={Object.keys(actionPolicies).map((key) => [key, t.actions[key].label])}
          onChange={(action) => setValue({ ...value, action })} />
        <small className="field-help">{t.actions[value.action].help}</small>
      </div>
      {!keep && <label className="field"><span>{t.words.changeToLabel}</span><input value={value.candidate} placeholder={t.words.changeToPlaceholder} onChange={(event) => setValue({ ...value, candidate: event.target.value })} /></label>}
      <div className="dialog-actions">
        <Button onClick={onCancel}>{t.cancel}</Button>
        <Button type="submit" variant="primary" disabled={!valid}>{t.save}</Button>
      </div>
    </form>
  );
}

const start = import.meta.env.DEV && !window.chrome?.webview
  ? import("./devHost.js").then((module) => module.installMockHost())
  : Promise.resolve();
start.then(() => createRoot(document.getElementById("root")).render(<App />));
