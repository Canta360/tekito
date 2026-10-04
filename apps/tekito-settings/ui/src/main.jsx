import React, { useCallback, useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { CapitataLogo } from "./CapitataLogo.jsx";
import { hostRequest, onHostStateChanged } from "./host.js";
import { messages, resolveLanguage, TextContext, useText } from "./i18n.js";
import { Backdrop, Button, Dialog, Glass, Icon, Keycaps, Row, Segmented, Tile, Toggle } from "./ui.jsx";
import "./styles.css";

const pages = [
  { id: "general", icon: "general" },
  { id: "switching", icon: "switching" },
  { id: "candidates", icon: "candidates" },
  { id: "english", icon: "english" },
  { id: "japanese", icon: "japanese" },
  { id: "special", icon: "special" },
  { id: "dictionary", icon: "dictionary" },
  { id: "about", icon: "about" },
];
// Links from before the pages were rearranged.
const pageAliases = { typing: "english" };

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
  candidateRows: 0,
  meaningsEnabled: true,
  japaneseSwitchOrder: 0,
  japaneseEnabled: true,
  modeIndicatorEnabled: true,
  toggleKey: 1,
  keyboardType: 0,
  periodOnEnter: false,
  japaneseSpaceWidth: 0,
  japanesePunctuation: 0,
  japaneseDigitWidth: 0,
  japaneseSymbolWidth: 0,
  advancedSettings: false,
  japanesePredictionEnabled: true,
  japaneseLiveConversion: false,
  japaneseIgnoreCapsLock: true,
  dateConversion: true,
  numberConversion: true,
  symbolConversion: true,
  calculatorEnabled: true,
  uiLanguage: 0,
  excludedApps: [],
  builtInExcludedApps: [],
};

// The switches a person would actually want to flip for English. Common
// misspellings and context ranking stay on (they only make corrections
// better) and are not shown; Japanese sounds only with advanced settings.
const englishFeatures = [
  { key: "correctionEnabled", icon: "spelling" },
  { key: "completionEnabled", icon: "completion" },
  { key: "japanesePhoneticSuggestionsEnabled", icon: "japanese", advanced: true },
];

// Special conversions (UserSettings), for English and Japanese alike.
const specialFeatures = [
  { key: "dateConversion", icon: "calendar" },
  { key: "numberConversion", icon: "numbers" },
  { key: "symbolConversion", icon: "symbols" },
  { key: "calculatorEnabled", icon: "calculator" },
];

// Candidate list look (UserSettings::candidateWindowStyle).
const candidateStyles = [{ value: 0, id: "glass" }, { value: 1, id: "simple" }];

// The input modes and their icons (assets/icons), with the versions for a
// dark background.
const modeTiles = {
  japanese: { image: "./assets/japanese.ico", imageDark: "./assets/japanese-dark.ico" },
  auto: { image: "./assets/auto.ico" },
  direct: { image: "./assets/direct.ico", imageDark: "./assets/direct-dark.ico" },
};

// The six engine policies boil down to three behaviors a person can tell
// apart: replace on Space, only suggest, or never touch the word.
const actionPolicies = { replace: "Correct spelling", suggest: "Suggest alternative", keep: "Protect original" };

function actionOf(policy) {
  if (policy === "Protect original") return "keep";
  if (policy === "Correct spelling" || policy === "Normalize") return "replace";
  return "suggest";
}

function pageFromHash() {
  const id = window.location.hash.slice(1);
  const page = pageAliases[id] || id;
  return pages.some((item) => item.id === page) ? page : "general";
}

// A file dropped anywhere but a drop area is ignored (WebView2 would
// otherwise open it in place of Settings).
for (const name of ["dragover", "drop"]) window.addEventListener(name, (event) => event.preventDefault());

function App() {
  // "#japanese" etc. opens a page directly.
  const [page, setPage] = useState(pageFromHash);
  const [connection, setConnection] = useState({ status: "loading", error: "" });
  const [settings, setSettings] = useState(initialSettings);
  const [mode, setMode] = useState("auto");
  const [runtime, setRuntime] = useState({ tsf: "Unavailable", dataPacks: "Unavailable", japaneseData: "", japanese: false });
  const [learning, setLearning] = useState({ enabled: true, count: 0 });
  const [dictionary, setDictionary] = useState([]);
  const [japaneseWords, setJapaneseWords] = useState([]);
  const [packs, setPacks] = useState([]);
  const [postalCodes, setPostalCodes] = useState({ installed: false });
  const [version, setVersion] = useState("");
  const [appearance, setAppearance] = useState({ accent: "#0078d4", systemLanguage: navigator.language.startsWith("ja") ? "ja" : "en" });
  const [modal, setModal] = useState(null);
  const [confirm, setConfirm] = useState(null);
  const [notice, setNotice] = useState(null);

  const t = messages[resolveLanguage(settings.uiLanguage, appearance.systemLanguage)];
  // Japanese is installed, and turned on in Settings.
  const japaneseInstalled = Boolean(runtime.japanese);
  const japanese = japaneseInstalled && settings.japaneseEnabled !== false;

  const applyState = useCallback((state) => {
    if (!state) return;
    setSettings((value) => ({ ...value, ...(state.settings || {}) }));
    setMode(["japanese", "direct"].includes(state.mode) ? state.mode : "auto");
    setRuntime((value) => ({ ...value, ...(state.runtime || {}) }));
    setLearning((value) => ({ ...value, ...(state.learning || {}) }));
    setDictionary(state.dictionary || []);
    setJapaneseWords(state.japaneseWords || []);
    setPacks(state.packs || []);
    if (state.postalCodes) setPostalCodes(state.postalCodes);
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

  const runAction = async (type, payload, successMessage, files) => {
    const response = await hostRequest(type, payload, files);
    if (response.ok) {
      applyState(response.state);
      if (successMessage) showNotice(successMessage);
    } else {
      showNotice(response.error || t.failed, "negative");
    }
    return response;
  };

  // A Japanese word, new or replacing the one the dialog was opened with.
  const saveJapaneseWord = async (word) => {
    const original = modal?.word || {};
    const response = await runAction("japaneseWords.save", {
      ...word,
      originalReading: original.reading || "",
      originalSurface: original.surface || "",
    }, modal?.mode === "edit" ? t.japaneseWords.updated : t.japaneseWords.added);
    if (response.ok) setModal(null);
    return response.ok;
  };

  const saveDictionary = async (entry, update) => {
    const response = await runAction(update ? "dictionary.update" : "dictionary.create", {
      ...entry,
      ...(update ? { id: modal?.entry?.id } : {}),
    }, update ? t.words.updated : t.words.added);
    if (response.ok) setModal(null);
    return response.ok;
  };

  // The Japanese page is there only while Japanese input is on.
  // Japanese only while it is on; Special conversions only with advanced
  // settings (everything on it is on by default).
  const advanced = Boolean(settings.advancedSettings);
  const shownPages = pages.filter((item) => (item.id !== "japanese" || japanese) && (item.id !== "special" || advanced));
  const current = shownPages.find((item) => item.id === page) || shownPages[0];

  return (
    <TextContext.Provider value={t}>
      <div className="app">
        <Backdrop />
        <Glass as="aside" className="sidebar">
          <div className="brand">
            <img src="./assets/tekito.ico" alt="" />
            <span>
              <Wordmark />
              <small>{t.settings}</small>
            </span>
          </div>
          <nav aria-label={t.settings}>
            {shownPages.map((item) => (
              <button key={item.id} type="button" className={`nav-item ${current.id === item.id ? "is-current" : ""}`}
                aria-current={current.id === item.id ? "page" : undefined} onClick={() => setPage(item.id)}>
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
            <div className="page" key={current.id}>
              {current.id === "general" && <GeneralPage advanced={advanced} mode={mode} japanese={japanese} japaneseInstalled={japaneseInstalled} settings={settings} changeMode={changeMode} setSetting={setSetting} runAction={runAction} />}
              {current.id === "switching" && <SwitchingPage advanced={advanced} japanese={japanese} japaneseKeyboard={Boolean(runtime.japaneseKeyboard)} settings={settings} setSetting={setSetting} />}
              {current.id === "candidates" && <CandidatesPage advanced={advanced} settings={settings} setSetting={setSetting} />}
              {current.id === "english" && <EnglishPage advanced={advanced} settings={settings} setSetting={setSetting} />}
              {current.id === "japanese" && <JapanesePage advanced={advanced} settings={settings} setSetting={setSetting} />}
              {current.id === "special" && <SpecialPage japanese={japanese} settings={settings} setSetting={setSetting} />}
              {current.id === "dictionary" && <DictionaryPage japanese={japaneseInstalled} postalCodes={postalCodes} japaneseWords={japaneseWords} learning={learning} settings={settings} setSetting={setSetting} dictionary={dictionary} setModal={setModal} setConfirm={setConfirm} runAction={runAction} />}
              {current.id === "about" && <AboutPage version={version} runtime={runtime} packs={packs} runAction={runAction} />}
            </div>
          )}
        </main>

        <Dialog open={Boolean(modal) && !modal.japanese} title={modal?.mode === "edit" ? t.words.editTitle : t.words.addTitle} onClose={() => setModal(null)}>
          <DictionaryForm entry={modal?.entry || {}} onCancel={() => setModal(null)} onSave={(entry) => saveDictionary(entry, modal?.mode === "edit")} />
        </Dialog>
        <Dialog open={Boolean(modal?.japanese)} title={modal?.mode === "edit" ? t.japaneseWords.editTitle : t.japaneseWords.addTitle} onClose={() => setModal(null)}>
          {modal?.japanese && <JapaneseWordForm word={modal.word || {}} onCancel={() => setModal(null)} onSave={saveJapaneseWord} />}
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

// General: which input mode is on, how to switch, and where TEKITO stays out.
// The TEKITO lettering, dark on light and light on dark.
function Wordmark({ large = false }) {
  return (
    <picture>
      <source media="(prefers-color-scheme: dark)" srcSet="./assets/tekito-wordmark-light.svg" />
      <img className={`wordmark ${large ? "large" : ""}`} src="./assets/tekito-wordmark-dark.svg" alt="TEKITO" />
    </picture>
  );
}

// A mode's taskbar icon, small.
function ModeIcon({ id }) {
  const t = useText();
  const tile = modeTiles[id];
  const name = t.modeNames[id];
  return (
    <picture title={name}>
      {tile.imageDark && <source media="(prefers-color-scheme: dark)" srcSet={tile.imageDark} />}
      <img className="mode-icon" src={tile.image} alt={name} />
    </picture>
  );
}

// The modes the switch key goes through, as their icons.
function ModeOrder({ ids, round }) {
  return (
    <span className="mode-order">
      {ids.map((id, index) => (
        <React.Fragment key={id}>
          {index > 0 && <span className="mode-arrow" aria-hidden="true">{round ? "→" : "⇄"}</span>}
          <ModeIcon id={id} />
        </React.Fragment>
      ))}
      {round && <span className="mode-arrow" aria-hidden="true">↺</span>}
    </span>
  );
}

// The keyboard's own switch key (UserSettings::toggleKey 1). Japanese key
// names come from the language's text: printed in Japanese on the keys, in
// letters in English.
const ownSwitchKey = (japaneseKeyboard, t) => (japaneseKeyboard ? [t.keyNames.hankaku] : ["Alt", "`"]);
const switchKeyCaps = (toggleKey, japaneseKeyboard, t) =>
  toggleKey === 1 ? ownSwitchKey(japaneseKeyboard, t)
  : toggleKey === 2 ? ["Ctrl", "Space"]
  : toggleKey === 3 ? ["Ctrl", "Shift", "Space"]
  : null;

// What TEKITO is doing overall: the mode, Japanese, the language, and (with
// advanced settings) how it starts and the apps where it stays off. The
// switch for advanced settings is last.
function GeneralPage({ advanced, mode, japanese, japaneseInstalled, settings, changeMode, setSetting, runAction }) {
  const t = useText();
  // UserSettings::uiLanguage.
  const languages = [[0, t.language.system], [1, "English"], [2, "日本語"]];
  // Japanese is a mode only when it is installed.
  const modes = japanese ? ["auto", "direct", "japanese"] : ["auto", "direct"];
  return (
    <>
      <div className={`tile-grid mode-grid ${japanese ? "three" : ""}`}>
        {modes.map((id) => (
          <Tile key={id} on={mode === id} image={modeTiles[id].image} imageDark={modeTiles[id].imageDark}
            title={t.modeNames[id]} description={t.modes[id]} status={mode === id ? t.inUse : undefined}
            onPress={() => changeMode(id)} />
        ))}
      </div>
      <Glass className="card">
        <Row title={t.japaneseInput.title}
          description={japaneseInstalled ? t.japaneseInput.description : t.japaneseInput.missing}>
          <Toggle label={t.japaneseInput.title} checked={japanese} disabled={!japaneseInstalled}
            onChange={(value) => setSetting("japaneseEnabled", value)} />
        </Row>
        <Row title={t.language.title} description={t.language.description}>
          <Segmented label={t.language.title} value={settings.uiLanguage} options={languages} onChange={(value) => setSetting("uiLanguage", value)} />
        </Row>
        {advanced && (
          <Row title={t.restoreMode.title} description={japanese ? t.restoreMode.descriptionJapanese : t.restoreMode.description}>
            <Toggle label={t.restoreMode.title} checked={settings.restoreLastInputMode} onChange={(value) => setSetting("restoreLastInputMode", value)} />
          </Row>
        )}
      </Glass>
      {advanced && <ExcludedApps settings={settings} runAction={runAction} />}
      <Glass className="card">
        <Row title={t.advanced.title} description={t.advanced.description}>
          <Toggle label={t.advanced.title} checked={advanced} onChange={(value) => setSetting("advancedSettings", value)} />
        </Row>
      </Glass>
    </>
  );
}

// How the mode is switched: the keyboard, the switch key and its order, the
// badge shown after a switch, and the keys for it.
function SwitchingPage({ advanced, japanese, japaneseKeyboard, settings, setSetting }) {
  const t = useText();
  // UserSettings::keyboardType, ::toggleKey and ::japaneseSwitchOrder. The
  // switch key choices follow the keyboard.
  const keyboards = [[0, t.keyboard.detect], [1, t.keyboard.japanese], [2, t.keyboard.us]];
  const toggleKeys = [1, 2, 3].map((value) => [value, <Keycaps keys={switchKeyCaps(value, japaneseKeyboard, t)} />])
    .concat([[0, t.switchKey.none]]);
  const orders = [
    [0, <ModeOrder ids={["japanese", "auto"]} />],
    [1, <ModeOrder ids={["japanese", "direct"]} />],
    [2, <ModeOrder ids={["japanese", "auto", "direct"]} round />],
  ];
  const switchKey = switchKeyCaps(settings.toggleKey, japaneseKeyboard, t);
  return (
    <>
      <Glass className="card">
        {advanced && (
          <Row title={t.keyboard.title} description={t.keyboard.description}>
            <Segmented label={t.keyboard.title} value={settings.keyboardType} options={keyboards} onChange={(value) => setSetting("keyboardType", value)} />
          </Row>
        )}
        <Row title={t.switchKey.title} description={japanese ? t.switchKey.descriptionJapanese : t.switchKey.description}>
          <Segmented label={t.switchKey.title} value={settings.toggleKey} options={toggleKeys} onChange={(value) => setSetting("toggleKey", value)} />
        </Row>
        {japanese && advanced && (
          <Row title={t.order.title} description={t.order.description}>
            <Segmented label={t.order.title} value={settings.japaneseSwitchOrder} options={orders} onChange={(value) => setSetting("japaneseSwitchOrder", value)} />
          </Row>
        )}
        <Row title={t.modeIndicator.title} description={t.modeIndicator.description}>
          <Toggle label={t.modeIndicator.title} checked={settings.modeIndicatorEnabled} onChange={(value) => setSetting("modeIndicatorEnabled", value)} />
        </Row>
      </Glass>
      <Glass className="card">
        <h2 className="card-title">{t.keys.title}</h2>
        <div className="keys">
          {switchKey && <Key label={japanese ? t.modeKeys.switch : t.modeKeys.switchEnglish} keys={switchKey} />}
          {japanese && japaneseKeyboard && (
            <>
              <Key label={t.modeKeys.henkan} keys={[t.keyNames.henkan]} />
              <Key label={t.modeKeys.muhenkan} keys={[t.keyNames.muhenkan]} />
              <Key label={t.modeKeys.hiragana} keys={[t.keyNames.hiragana]} />
            </>
          )}
          <Key label={t.modeKeys.taskbar} keys={[t.modeKeys.taskbarButton]} />
        </div>
      </Glass>
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

// The candidate list, shared by English and Japanese.
function CandidatesPage({ advanced, settings, setSetting }) {
  const t = useText();
  // UserSettings::candidateRows.
  const rowCounts = [[0, t.rows.automatic], [5, "5"], [7, "7"], [9, "9"]];
  const shown = settings.candidateWindowEnabled;
  return (
    <>
      <div className={`tile-grid mode-grid ${shown ? "" : "is-muted"}`}>
        {candidateStyles.map((style) => (
          <Tile key={style.value} on={settings.candidateWindowStyle === style.value} title={t.styles[style.id].title}
            description={t.styles[style.id].description} status={settings.candidateWindowStyle === style.value ? t.inUse : undefined}
            art={<MiniList variant={style.id} />} onPress={() => setSetting("candidateWindowStyle", style.value)} />
        ))}
      </div>
      {advanced && (
        <Glass className="card">
          <Row title={t.candidateList.title} description={t.candidateList.description}>
            <Toggle label={t.candidateList.title} checked={shown} onChange={(value) => setSetting("candidateWindowEnabled", value)} />
          </Row>
          <Row title={t.rows.title} description={t.rows.description}>
            <Segmented label={t.rows.title} value={settings.candidateRows} options={rowCounts} onChange={(value) => setSetting("candidateRows", value)} />
          </Row>
          <Row title={t.meanings.title} description={t.meanings.description}>
            <Toggle label={t.meanings.title} checked={settings.meaningsEnabled} onChange={(value) => setSetting("meaningsEnabled", value)} />
          </Row>
        </Glass>
      )}
    </>
  );
}

// English: what Auto does as you type.
function EnglishPage({ advanced, settings, setSetting }) {
  const t = useText();
  const expressionRanges = t.casual.ranges.map((label, value) => [value, label]);
  const features = englishFeatures.filter((feature) => advanced || !feature.advanced);
  return (
    <>
      <div className={`tile-grid feature-grid ${features.length === 3 ? "three" : "two"}`}>
        {features.map((feature) => (
          <Tile key={feature.key} on={Boolean(settings[feature.key])} icon={feature.icon} title={t.features[feature.key].title}
            description={t.features[feature.key].description} status={settings[feature.key] ? t.on : t.off}
            onPress={() => setSetting(feature.key, !settings[feature.key])} />
        ))}
      </div>
      <Glass className="card">
        <Row title={t.casual.title} description={t.casual.description}>
          <Segmented label={t.casual.title} value={settings.socialExpressionRange} options={expressionRanges} onChange={(value) => setSetting("socialExpressionRange", value)} />
        </Row>
        {advanced && (
          <Row title={t.period.title} description={t.period.description}>
            <Toggle label={t.period.title} checked={settings.periodOnEnter} onChange={(value) => setSetting("periodOnEnter", value)} />
          </Row>
        )}
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

// Japanese: how romaji becomes Japanese, when Japanese is installed.
// Shown only while Japanese input is on (see General).
function JapanesePage({ advanced, settings, setSetting }) {
  const t = useText();
  const j = t.japanese;
  // UserSettings::japaneseSpaceWidth, ::japanesePunctuation, ::japaneseDigitWidth
  // and ::japaneseSymbolWidth (half-width first in both).
  const spaces = [[0, j.space.follow], [1, j.space.half], [2, j.space.full]];
  const marks = [[0, "、。"], [1, "，．"], [2, "，。"], [3, "、．"]];
  const digits = [[0, "123"], [1, "１２３"]];
  const symbols = [[1, "!?"], [0, "！？"]];
  return (
    <>
      <Glass className="card">
        <Row title={j.live.title}>
          <Toggle label={j.live.title} checked={settings.japaneseLiveConversion} onChange={(value) => setSetting("japaneseLiveConversion", value)} />
        </Row>
        <Row title={j.prediction.title} description={j.prediction.description}>
          <Toggle label={j.prediction.title} checked={settings.japanesePredictionEnabled} onChange={(value) => setSetting("japanesePredictionEnabled", value)} />
        </Row>
        <Row title={j.punctuation.title} description={j.punctuation.description}>
          <Segmented label={j.punctuation.title} value={settings.japanesePunctuation} options={marks} onChange={(value) => setSetting("japanesePunctuation", value)} />
        </Row>
        {advanced && (
          <>
            <Row title={j.space.title} description={j.space.description}>
              <Segmented label={j.space.title} value={settings.japaneseSpaceWidth} options={spaces} onChange={(value) => setSetting("japaneseSpaceWidth", value)} />
            </Row>
            <Row title={j.digits.title} description={j.digits.description}>
              <Segmented label={j.digits.title} value={settings.japaneseDigitWidth} options={digits} onChange={(value) => setSetting("japaneseDigitWidth", value)} />
            </Row>
            <Row title={j.symbols.title} description={j.symbols.description}>
              <Segmented label={j.symbols.title} value={settings.japaneseSymbolWidth} options={symbols} onChange={(value) => setSetting("japaneseSymbolWidth", value)} />
            </Row>
            <Row title={j.capsLock.title} description={j.capsLock.description}>
              <Toggle label={j.capsLock.title} checked={settings.japaneseIgnoreCapsLock} onChange={(value) => setSetting("japaneseIgnoreCapsLock", value)} />
            </Row>
          </>
        )}
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
          <Key label={j.keys.undo} keys={["Ctrl", "Backspace"]} />
          <Key label={j.keys.forget} keys={["Ctrl", "Delete"]} />
        </div>
      </Glass>
    </>
  );
}

// Special conversions: what else becomes a candidate, in both languages.
// Examples show today's date and time; Japanese ones only with Japanese on.
function SpecialPage({ japanese, settings, setSetting }) {
  const t = useText();
  const s = t.special;
  const now = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  const englishDate = now.toLocaleDateString("en-US", { month: "long", day: "numeric", year: "numeric" });
  const englishTime = now.toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit" });
  const japaneseDate = `${now.getFullYear()}/${pad(now.getMonth() + 1)}/${pad(now.getDate())}`;
  const examples = [
    { language: "english", typed: "today", result: englishDate, key: "dateConversion" },
    { language: "english", typed: "now", result: englishTime, key: "dateConversion" },
    { language: "english", typed: "(c)", result: "©", key: "symbolConversion" },
    { language: "english", typed: "->", result: "→", key: "symbolConversion" },
    { language: "english", typed: "1+2=", result: "3", key: "calculatorEnabled" },
    { language: "japanese", typed: "きょう", result: japaneseDate, key: "dateConversion" },
    { language: "japanese", typed: "1234", result: "千二百三十四", key: "numberConversion" },
    { language: "japanese", typed: "やじるし", result: "→", key: "symbolConversion" },
    { language: "japanese", typed: "1+2=", result: "3", key: "calculatorEnabled" },
  ].filter((example) => japanese || example.language === "english");
  const groups = japanese ? ["english", "japanese"] : ["english"];
  return (
    <>
      <div className="tile-grid feature-grid">
        {specialFeatures.map((feature) => (
          <Tile key={feature.key} on={Boolean(settings[feature.key])} icon={feature.icon} title={s[feature.key].title}
            description={s[feature.key].description} status={settings[feature.key] ? t.on : t.off}
            onPress={() => setSetting(feature.key, !settings[feature.key])} />
        ))}
      </div>
      <Glass className="card">
        <h2 className="card-title">{s.examples.title}</h2>
        <p className="card-note">{s.examples.description}</p>
        <div className="example-groups">
          {groups.map((language) => (
            <section key={language} className="example-group">
              <h3>{s.examples[language]}</h3>
              {examples.filter((example) => example.language === language).map((example) => (
                <div key={example.typed} className={`example ${settings[example.key] ? "" : "is-off"}`}>
                  <span className="example-typed">{example.typed}</span>
                  <span className="example-arrow" aria-hidden="true">{"→"}</span>
                  <span className="example-result">{example.result}</span>
                </div>
              ))}
            </section>
          ))}
        </div>
      </Glass>
    </>
  );
}

// Postal codes to addresses, a dictionary the user adds: Japan Post's data,
// downloaded and dropped here (or chosen), since it changes every month.
function PostalCodes({ postalCodes, runAction }) {
  const t = useText();
  const p = t.postalCodes;
  const [dragging, setDragging] = useState(false);
  const [busy, setBusy] = useState(false);
  const add = async (type, files) => {
    setBusy(true);
    await runAction(type, {}, p.added, files);
    setBusy(false);
  };
  const drop = (event) => {
    event.preventDefault();
    setDragging(false);
    const file = event.dataTransfer.files[0];
    if (file && !busy) add("postalCodes.import", [file]);
  };
  const [year, month] = String(postalCodes.version || "").split(".");
  return (
    <Glass className={`card postal ${dragging ? "is-dragging" : ""}`}
      onDragOver={(event) => { event.preventDefault(); setDragging(true); }}
      onDragLeave={() => setDragging(false)} onDrop={drop}>
      <div className="card-head">
        <div>
          <h2 className="card-title">{p.title}</h2>
          <p className="muted">{postalCodes.installed ? p.installed(year, month, postalCodes.count) : p.description}</p>
        </div>
        {postalCodes.installed && (
          <Button icon="trash" onClick={() => runAction("postalCodes.remove", {}, p.removed)}>{t.words.remove}</Button>
        )}
      </div>
      <div className="postal-steps">
        <div className="postal-step">
          <span className="step-number">1</span>
          <p>{p.download}</p>
          <Button onClick={() => runAction("postalCodes.open", {})}>{p.openPage}</Button>
        </div>
        <div className="postal-step drop-zone">
          <span className="step-number">2</span>
          <p>{busy ? p.adding : p.drop}</p>
          <Button icon="folder" disabled={busy} onClick={() => add("postalCodes.browse")}>{p.choose}</Button>
        </div>
      </div>
    </Glass>
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
  return <div className="key"><Keycaps keys={keys} /><small>{label}</small></div>;
}

// Dictionary and learning: your words, and what TEKITO learned from you.
function DictionaryPage({ japanese, postalCodes, japaneseWords, learning, settings, setSetting, dictionary, setModal, setConfirm, runAction }) {
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
  const askDeleteJapanese = (word) => setConfirm({
    title: t.japaneseWords.removeTitle,
    message: t.japaneseWords.removeMessage(word.surface),
    confirmLabel: t.words.remove,
    action: () => runAction("japaneseWords.delete", { reading: word.reading, surface: word.surface }, t.japaneseWords.removed),
  });
  const askForgetJapanese = () => setConfirm({
    title: t.learning.japaneseConfirmTitle,
    message: t.learning.japaneseConfirmMessage,
    confirmLabel: t.learning.forget,
    action: () => runAction("japaneseLearning.clear", {}, t.learning.cleared),
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
        {japanese && (
          <Row title={t.learning.japaneseTitle} description={t.learning.japaneseDescription}>
            <Button variant="quiet" onClick={askForgetJapanese}>{t.learning.forget}</Button>
          </Row>
        )}
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

      {japanese && (
        <Glass className="card">
          <div className="card-head">
            <div>
              <h2 className="card-title">{t.japaneseWords.title}</h2>
              <p className="muted">{t.japaneseWords.description}</p>
            </div>
            <Button variant="primary" icon="plus" onClick={() => setModal({ japanese: true, mode: "new", word: {} })}>{t.add}</Button>
          </div>
          <ul className="word-list" aria-label={t.japaneseWords.title}>
            {japaneseWords.map((word) => (
              <li key={`${word.reading}\u0001${word.surface}`} className="word">
                <button type="button" className="word-main" onClick={() => setModal({ japanese: true, mode: "edit", word })}>
                  <b>{word.reading}</b>
                  <Icon name="arrow" size={15} className="word-arrow" />
                  <span>{word.surface}</span>
                  {word.action !== "suppress" && <span className="badge">{t.japaneseWords.kinds[word.kind] || word.kind}</span>}
                  {word.action && word.action !== "first" && (
                    <span className={`badge ${word.action}`}>{t.japaneseWords.actions[word.action].label}</span>
                  )}
                </button>
                <button type="button" className="icon-btn" aria-label={t.words.removeLabel(word.surface)} onClick={() => askDeleteJapanese(word)}>
                  <Icon name="trash" size={16} />
                </button>
              </li>
            ))}
          </ul>
          {japaneseWords.length === 0 && <div className="empty">{t.japaneseWords.empty}</div>}
        </Glass>
      )}
      {japanese && <PostalCodes postalCodes={postalCodes} runAction={runAction} />}
    </>
  );
}

// One line of the About status: what it is, how it stands.
function StatusLine({ title, detail, state, stateLabel }) {
  return (
    <div className="status-line">
      <span><b>{title}</b><small>{detail}</small></span>
      <span className={`state ${state}`}><span className="dot" />{stateLabel}</span>
    </div>
  );
}

function AboutPage({ version, runtime, packs, runAction }) {
  const t = useText();
  const [license, setLicense] = useState(null);
  const openLicense = async () => {
    const response = await hostRequest("license.get");
    setLicense(response.ok ? response.text : t.about.licenseMissing);
  };
  const english = packs.filter((pack) => pack.group !== "japanese");
  const japanesePacks = packs.filter((pack) => pack.group === "japanese");
  const englishProblems = english.filter((pack) => !pack.valid);
  const japaneseValid = japanesePacks.filter((pack) => pack.valid);
  // Japanese not installed at all is a choice, not a problem.
  const japaneseInstalled = japaneseValid.length > 0;
  const japaneseProblems = japaneseInstalled ? japanesePacks.filter((pack) => !pack.valid) : [];
  const installed = runtime.tsf === "Loaded";
  return (
    <>
      <Glass className="card about-hero">
        <img className="about-icon" src="./assets/tekito.ico" alt="" />
        <div>
          <Wordmark large />
          <p>{t.about.tagline}</p>
        </div>
        <span className="version"><small>{t.about.version}</small><b>{version}</b></span>
      </Glass>

      <Glass className="card">
        <StatusLine title={t.about.inputMethod} detail={installed ? t.about.installed : t.about.notInstalled}
          state={installed ? "" : "problem"} stateLabel={installed ? t.about.ready : t.about.problem} />
        <StatusLine title={t.about.englishData}
          detail={englishProblems.length === 0 ? t.about.allPacks(english.length) : t.about.somePacks(runtime.dataPacks)}
          state={englishProblems.length ? "problem" : ""} stateLabel={englishProblems.length ? t.about.problem : t.about.ready} />
        <StatusLine title={t.about.japaneseData}
          detail={!japaneseInstalled ? t.about.japaneseMissing
            : japaneseProblems.length === 0 ? t.about.allPacks(japanesePacks.length) : t.about.somePacks(runtime.japaneseData)}
          state={!japaneseInstalled ? "off" : japaneseProblems.length ? "problem" : ""}
          stateLabel={!japaneseInstalled ? t.about.notInstalledState : japaneseProblems.length ? t.about.problem : t.about.ready} />
        {[...englishProblems, ...japaneseProblems].map((pack) => (
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

// The kinds a Japanese word can be (JapaneseUserDictionary.h), in the order
// the form lists them.
const japaneseKinds = ["noun", "proper-noun", "person", "surname", "given-name", "place", "organization",
  "suru-noun", "symbol", "interjection"];
// What converting does with the word (JapaneseUserDictionary.h), like the
// English words' Replace / Suggest / Keep as typed.
const japaneseActions = ["first", "suggest", "suppress"];
// A reading: hiragana, or katakana (the host makes it hiragana).
const readingPattern = /^[\u3041-\u3096\u30A1-\u30F6\u30FC\u309D\u309E]+$/;

function JapaneseWordForm({ word, onSave, onCancel }) {
  const t = useText();
  const w = t.japaneseWords;
  const [value, setValue] = useState({ reading: word.reading || "", surface: word.surface || "", kind: word.kind || "noun",
    action: word.action || "first" });
  const reading = value.reading.trim();
  const readingOk = readingPattern.test(reading);
  const valid = readingOk && value.surface.trim();
  const submit = (event) => {
    event.preventDefault();
    if (valid) onSave({ reading, surface: value.surface.trim(), kind: value.kind, action: value.action });
  };
  return (
    <form className="form" onSubmit={submit}>
      <label className="field">
        <span>{w.readingLabel}</span>
        <input autoFocus value={value.reading} placeholder={w.readingPlaceholder} lang="ja"
          onChange={(event) => setValue({ ...value, reading: event.target.value })} />
        <small className="field-help">{reading && !readingOk ? w.readingInvalid : w.readingHelp}</small>
      </label>
      <label className="field">
        <span>{w.surfaceLabel}</span>
        <input value={value.surface} placeholder={w.surfacePlaceholder} lang="ja"
          onChange={(event) => setValue({ ...value, surface: event.target.value })} />
      </label>
      <div className="field">
        <span>{w.actionLabel}</span>
        <Segmented label={w.actionLabel} value={value.action} options={japaneseActions.map((action) => [action, w.actions[action].label])}
          onChange={(action) => setValue({ ...value, action })} />
        <small className="field-help">{w.actions[value.action].help}</small>
      </div>
      {value.action !== "suppress" && (
        <label className="field">
          <span>{w.kindLabel}</span>
          <select value={value.kind} onChange={(event) => setValue({ ...value, kind: event.target.value })}>
            {japaneseKinds.map((kind) => <option key={kind} value={kind}>{w.kinds[kind]}</option>)}
          </select>
        </label>
      )}
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
