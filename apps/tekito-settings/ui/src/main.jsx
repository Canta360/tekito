import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { CapitataLogo } from "./CapitataLogo.jsx";
import { hostRequest, onHostStateChanged } from "./host.js";
import { messages, resolveLanguage, TextContext, useText } from "./i18n.js";
import {
  Button, Card, Check, ChoiceRow, Details, Dialog, Heading, Keycaps, LinkRow, ModeIcon, Row, Segmented, Select,
  Slider, Symbol, Toggle, Wordmark,
} from "./ui.jsx";
import "./styles.css";

// The pages, in the order of the list. Japanese and Mix are there while
// their mode is on.
const pageIds = ["general", "japanese", "mixed", "auto", "dictionary", "about"];
// Links from before the pages were rearranged.
const pageAliases = { switching: "general", candidates: "general", english: "auto", typing: "auto", special: "general" };

// Mirrors the defaults in src/UserData/UserSettings.h; only shown until the
// host's first reply arrives.
const initialSettings = {
  startMode: 0,
  switchOrder: 1234,
  switchModes: 7,
  correctionEnabled: true,
  completionEnabled: true,
  candidateWindowEnabled: true,
  learningEnabled: true,
  japanesePhoneticSuggestionsEnabled: true,
  socialExpressionRange: 1,
  socialPersonalization: 1,
  candidateWindowStyle: 0,
  candidateRows: 0,
  meaningsEnabled: true,
  meaningsInUiLanguage: true,
  mixPredictionEnabled: true,
  mixFirstPageSymbol: true,
  mixCorrection: true,
  japaneseEnabled: true,
  modeIndicatorEnabled: true,
  modeUnderlineColors: true,
  modeBadgeOnFocus: true,
  modePerApp: false,
  toggleKey: 1,
  keyboardType: 0,
  periodOnEnter: false,
  nextWordPrediction: true,
  doubledWordCheck: true,
  englishSpelling: 0,
  recheckPreviousWord: true,
  bracketPairs: false,
  doubleSpacePeriod: false,
  smartPunctuation: false,
  japaneseSpaceWidth: 0,
  japanesePunctuation: 0,
  japaneseDigitWidth: 0,
  japaneseNumberStyle: 1,
  japaneseRoughNumbersKanji: true,
  japaneseKanjiRange: 0,
  japaneseKanjiGrade: 6,
  japaneseKanjiNames: false,
  japaneseSymbolWidth: 0,
  japaneseLetterWidth: 1,
  keepRecentConversions: true,
  japanesePredictionEnabled: true,
  japaneseLiveConversion: false,
  japaneseSentenceCheck: true,
  japaneseFirstPageSymbol: true,
  japaneseTypedForms: true,
  japaneseLiveRefine: true,
  japaneseKanaInput: false,
  japaneseKanaInputKey: true,
  mixSingleVowelKana: true,
  mixLiveConversion: false,
  noLearningInPrivate: true,
  mixedTypingEnabled: false,
  japaneseEnglishSpacing: false,
  japaneseIgnoreCapsLock: true,
  dateConversion: true,
  numberConversion: true,
  symbolConversion: true,
  calculatorEnabled: true,
  uiLanguage: 0,
  excludedApps: [],
  builtInExcludedApps: [],
};

// The modes in UserSettings::switchOrder's digits, and their switchModes bits.
const orderDigits = { 1: "japanese", 2: "mixed", 3: "auto", 4: "direct" };
const modeBits = { japanese: 1, mixed: 2, auto: 4, direct: 8 };
const orderIds = (order) => String(order).split("").map((digit) => orderDigits[digit]).filter(Boolean);
const orderValue = (ids) => Number(ids.map((id) => Object.keys(orderDigits).find((digit) => orderDigits[digit] === id)).join(""));

// The English words' six engine policies come down to three behaviors.
const actionPolicies = { replace: "Correct spelling", suggest: "Suggest alternative", keep: "Protect original" };
function actionOf(policy) {
  if (policy === "Protect original") return "keep";
  if (policy === "Correct spelling" || policy === "Normalize") return "replace";
  return "suggest";
}

function pageFromHash() {
  const id = window.location.hash.slice(1);
  return pageAliases[id] || (pageIds.includes(id) ? id : "general");
}

// A file dropped anywhere but a drop area is ignored (WebView2 would
// otherwise open it in place of Settings).
for (const name of ["dragover", "drop"]) window.addEventListener(name, (event) => event.preventDefault());

function App() {
  const [page, setPage] = useState(pageFromHash);
  const [connection, setConnection] = useState({ status: "loading", error: "" });
  const [settings, setSettings] = useState(initialSettings);
  const [mode, setMode] = useState("auto");
  const [runtime, setRuntime] = useState({ tsf: "Unavailable", japanese: false });
  const [learning, setLearning] = useState({ enabled: true, count: 0 });
  const [dictionary, setDictionary] = useState([]);
  const [japaneseWords, setJapaneseWords] = useState([]);
  const [romaji, setRomaji] = useState([]);
  const [packs, setPacks] = useState([]);
  const [postalCodes, setPostalCodes] = useState({ installed: false });
  const [version, setVersion] = useState("");
  const [appearance, setAppearance] = useState({ accent: "#0067c0", systemLanguage: navigator.language.startsWith("ja") ? "ja" : "en" });
  const [modal, setModal] = useState(null);
  const [confirm, setConfirm] = useState(null);
  const [keys, setKeys] = useState(null);
  const [notice, setNotice] = useState(null);

  const t = messages[resolveLanguage(settings.uiLanguage, appearance.systemLanguage)];
  const japaneseInstalled = Boolean(runtime.japanese);
  const japanese = japaneseInstalled && settings.japaneseEnabled !== false;
  const mixed = japaneseInstalled && Boolean(settings.mixedTypingEnabled);

  const applyState = useCallback((state) => {
    if (!state) return;
    setSettings((value) => ({ ...value, ...(state.settings || {}) }));
    setMode(["japanese", "mixed", "direct"].includes(state.mode) ? state.mode : "auto");
    setRuntime((value) => ({ ...value, ...(state.runtime || {}) }));
    setLearning((value) => ({ ...value, ...(state.learning || {}) }));
    setDictionary(state.dictionary || []);
    setJapaneseWords(state.japaneseWords || []);
    setRomaji(state.romaji || []);
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
    // The host pushes changes made elsewhere (e.g. the taskbar button).
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
      const message = typeof successMessage === "function" ? successMessage(response) : successMessage;
      if (message) showNotice(message);
    } else {
      showNotice(response.error || t.failed, "negative");
    }
    return response;
  };

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

  const shownPages = pageIds.filter((id) => (id !== "japanese" || japanese) && (id !== "mixed" || mixed));
  const current = shownPages.includes(page) ? page : "general";
  const open = (id) => {
    setPage(id);
    document.querySelector(".content")?.scrollTo(0, 0);
  };
  const pageProps = { settings, setSetting, runAction, setConfirm, showKeys: setKeys, open };

  return (
    <TextContext.Provider value={t}>
      <div className="app">
        <aside className="sidebar">
          <div className="brand"><img src="./assets/tekito.ico" alt="" /><Wordmark />{t.settings}</div>
          <SettingsSearch pages={shownPages} open={open} japaneseInstalled={japaneseInstalled} />
          <nav aria-label={t.settings} style={{ display: "contents" }}>
            {shownPages.filter((id) => id !== "about").map((id) => (
              <NavItem key={id} id={id} current={current === id} onClick={() => open(id)} />
            ))}
            <span className="nav-spacer" />
            <NavItem id="about" current={current === "about"} onClick={() => open("about")} />
          </nav>
        </aside>

        <main className="content">
          <div className="page" key={current}>
            <h1 className="page-title">{t.pages[current]}</h1>
            {connection.status !== "ready" ? (
              <Card className="state-card" role="status">
                <b>{connection.status === "loading" ? t.loading.title : t.unavailable.title}</b>
                <span className="muted">{connection.status === "loading" ? t.loading.body : connection.error || t.unavailable.body}</span>
                {connection.status === "error" && <Button variant="primary" onClick={reload}>{t.tryAgain}</Button>}
              </Card>
            ) : (
              <>
                {current === "general" && <GeneralPage {...pageProps} mode={mode} japaneseInstalled={japaneseInstalled}
                  japaneseKeyboard={Boolean(runtime.japaneseKeyboard)} changeMode={changeMode} />}
                {current === "japanese" && <JapanesePage {...pageProps} romaji={romaji} postalCodes={postalCodes} />}
                {current === "mixed" && <MixedPage {...pageProps} />}
                {current === "auto" && <AutoPage {...pageProps} />}
                {current === "dictionary" && <DictionaryPage {...pageProps} japanese={japaneseInstalled} learning={learning}
                  dictionary={dictionary} japaneseWords={japaneseWords} setModal={setModal} />}
                {current === "about" && <AboutPage {...pageProps} version={version} runtime={runtime} packs={packs} japanese={japaneseInstalled} />}
              </>
            )}
          </div>
        </main>

        <Dialog open={Boolean(modal) && !modal.japanese} title={modal?.mode === "edit" ? t.words.editTitle : t.words.addTitle} onClose={() => setModal(null)}>
          <DictionaryForm entry={modal?.entry || {}} onCancel={() => setModal(null)} onSave={(entry) => saveDictionary(entry, modal?.mode === "edit")} />
        </Dialog>
        <Dialog open={Boolean(modal?.japanese)} title={modal?.mode === "edit" ? t.japaneseWords.editTitle : t.japaneseWords.addTitle} onClose={() => setModal(null)}>
          {modal?.japanese && <JapaneseWordForm word={modal.word || {}} onCancel={() => setModal(null)} onSave={saveJapaneseWord} />}
        </Dialog>
        <Dialog open={Boolean(confirm)} title={confirm?.title} onClose={() => setConfirm(null)}>
          {confirm?.message && <p className="dialog-message">{confirm.message}</p>}
          {confirm?.link && <button type="button" className="dialog-link" onClick={() => { const link = confirm.link; setConfirm(null); link.action(); }}>{confirm.link.label}</button>}
          <div className="dialog-actions">
            <Button variant={confirm?.danger ? "danger" : "primary"} onClick={async () => { const action = confirm.action; setConfirm(null); await action(); }}>{confirm?.confirmLabel}</Button>
            <Button onClick={() => setConfirm(null)}>{t.cancel}</Button>
          </div>
        </Dialog>
        <Dialog open={Boolean(keys)} title={t.keysTitle} onClose={() => setKeys(null)}>
          {keys && <KeyList page={keys} japaneseKeyboard={Boolean(runtime.japaneseKeyboard)} settings={settings} japanese={japanese} />}
          <div className="dialog-actions"><Button variant="primary" onClick={() => setKeys(null)}>{t.close}</Button></div>
        </Dialog>
        {notice && <div key={notice.key} className={`toast ${notice.tone}`} role="status" aria-live="polite">{notice.message}</div>}
      </div>
    </TextContext.Provider>
  );
}

const navIcons = { general: "settings", dictionary: "menu_book", about: "info" };

function NavItem({ id, current, onClick }) {
  const t = useText();
  return (
    <button type="button" className={`nav-item ${current ? "is-current" : ""}`} aria-current={current ? "page" : undefined} onClick={onClick}>
      {navIcons[id] ? <Symbol name={navIcons[id]} size={18} /> : <ModeIcon id={id} size={18} />}
      {t.pages[id]}
    </button>
  );
}

// The settings by name, for the search box: [page, name, under 詳細].
function searchIndex(t, pages, japaneseInstalled) {
  const g = t.general;
  const j = t.japanese;
  const m = t.mixed;
  const a = t.auto;
  const d = t.dictionary;
  const entries = [
    ["general", g.switchKey.title], ["general", g.order.title], ["general", g.indicator.title], ["general", g.candidates],
    ["general", g.rows.title], ["general", g.meanings.title], ["general", g.startMode.title, true], ["general", g.modePerApp.title, true],
    ["general", g.underline.title, true], ["general", g.keyboard.title, true], ["general", g.date.title, true],
    ["general", g.symbols.title, true], ["general", g.calculator.title, true], ["general", g.candidateList.title, true],
    ["general", g.brackets.title, true], ["general", g.language.title, true], ["general", g.excluded.title, true],
    ["japanese", j.live.title], ["japanese", j.prediction.title], ["japanese", j.numbers.title], ["japanese", j.kanji.title],
    ["japanese", j.romaji.title], ["japanese", j.postal.title], ["japanese", j.kanaInput.title, true], ["japanese", j.capsLock.title, true],
    ["japanese", j.punctuation.title, true], ["japanese", j.space.title, true], ["japanese", j.letters.title, true],
    ["japanese", j.symbols.title, true], ["japanese", j.spacing.title, true], ["japanese", j.sentenceCheck.title, true],
    ["japanese", j.firstPageSymbol.title, true], ["japanese", j.typedForms.title, true],
    ["mixed", m.live.title], ["mixed", m.prediction.title], ["mixed", m.singleVowel.title], ["mixed", m.correction.title],
    ["mixed", m.firstPageSymbol.title, true],
    ["auto", a.correction.title], ["auto", a.spelling.title], ["auto", a.completion.title], ["auto", a.next.title], ["auto", a.casual.title],
    ["auto", a.doubled.title, true], ["auto", a.recheck.title, true], ["auto", a.phonetic.title, true], ["auto", a.period.title, true],
    ["auto", a.doubleSpace.title, true], ["auto", a.smart.title, true],
    ["dictionary", d.japaneseWords.title], ["dictionary", d.englishWords.title], ["dictionary", d.files.title],
    ["dictionary", d.learn.title], ["dictionary", d.learned.title], ["dictionary", d.private.title],
    ["about", t.about.report.title], ["about", t.about.bug.title], ["about", t.about.feature.title], ["about", t.about.releases.title],
    ["about", t.about.diagnostics], ["about", t.about.license],
  ];
  return entries.filter(([page, title]) => pages.includes(page) &&
    (japaneseInstalled || (title !== d.japaneseWords.title && title !== t.about.report.title)));
}

function SettingsSearch({ pages, open, japaneseInstalled }) {
  const t = useText();
  const [query, setQuery] = useState("");
  const needle = query.trim().toLowerCase();
  const results = needle ? searchIndex(t, pages, japaneseInstalled).filter(([, title]) => title.toLowerCase().includes(needle)).slice(0, 8) : [];
  const go = ([page, , detailed]) => {
    if (detailed) {
      try {
        window.localStorage.setItem(`tekito.details.${page}`, "1");
      } catch {
        // Opening 詳細 for it is a convenience only.
      }
    }
    setQuery("");
    open(page);
  };
  return (
    <div className="search-box">
      <input aria-label={t.search} placeholder={t.search} value={query} onChange={(event) => setQuery(event.target.value)}
        onKeyDown={(event) => { if (event.key === "Enter" && results[0]) go(results[0]); if (event.key === "Escape") setQuery(""); }} />
      <Symbol name="search" size={16} />
      {needle && (
        <div className="search-results">
          {results.length === 0 ? <p>{t.noResults}</p> : results.map((entry) => (
            <button key={`${entry[0]}-${entry[1]}`} type="button" onClick={() => go(entry)}>
              {entry[1]}<small>{t.pages[entry[0]]}</small>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

// The keyboard's own switch key (UserSettings::toggleKey 1).
const ownSwitchKey = (japaneseKeyboard, t) => (japaneseKeyboard ? [t.keyNames.hankaku] : ["Alt", "`"]);
const switchKeyCaps = (toggleKey, japaneseKeyboard, t) =>
  toggleKey === 1 ? ownSwitchKey(japaneseKeyboard, t)
  : toggleKey === 2 ? ["Ctrl", "Space"]
  : toggleKey === 3 ? ["Ctrl", "Shift", "Space"]
  : null;

function KeyList({ page, japaneseKeyboard, settings, japanese }) {
  const t = useText();
  const rows = [];
  if (page === "general") {
    const switchKey = switchKeyCaps(settings.toggleKey, japaneseKeyboard, t);
    if (switchKey) rows.push([japanese ? t.modeKeys.switch : t.modeKeys.switchEnglish, switchKey]);
    if (japanese && japaneseKeyboard) {
      rows.push([t.modeKeys.henkan, [t.keyNames.henkan]], [t.modeKeys.muhenkan, [t.keyNames.muhenkan]], [t.modeKeys.hiragana, [t.keyNames.hiragana]]);
    }
    rows.push([t.modeKeys.taskbar, [t.modeKeys.taskbarButton]]);
  } else if (page === "japanese") {
    const k = t.japanese.keys;
    rows.push([k.convert, ["Space"]], [k.phrases, ["←", "→"]], [k.resize, ["Shift", "←", "→"]], [k.predictions, ["Tab", "↓"]],
      [k.kana, ["F6", "F7", "F8"]], [k.letters, ["F9", "F10"]], [k.ctrlForms, ["Ctrl", "U I O P T"]], [k.english, ["Shift", "A-Z"]],
      [k.back, ["Esc"]], [k.undo, ["Ctrl", "Backspace"]], [k.forget, ["Ctrl", "Delete"]]);
  } else {
    const k = t.auto.keys;
    rows.push([k.next, ["Space"]], [k.previous, ["Shift", "Space"]], [k.browse, ["Tab", "↑", "↓"]], [k.undo, ["Backspace"]], [k.keep, ["Esc"]]);
  }
  return (
    <ul className="list key-list">
      {rows.map(([label, caps]) => <li key={label}><span className="text">{label}</span><Keycaps keys={caps} /></li>)}
    </ul>
  );
}

function KeysCard({ page, showKeys }) {
  const t = useText();
  return <Card label={t.keysTitle}><LinkRow icon="keyboard_command_key" title={t.keysRow} onClick={() => showKeys(page)} /></Card>;
}

// ---------------------------------------------------------------------------
// 一般: the modes, switching, the candidate list, and what applies to all.

function GeneralPage({ settings, setSetting, runAction, showKeys, mode, changeMode, japaneseInstalled, japaneseKeyboard }) {
  const t = useText();
  const g = t.general;
  const japanese = japaneseInstalled && settings.japaneseEnabled !== false;
  const mixed = japaneseInstalled && Boolean(settings.mixedTypingEnabled);
  const modes = japaneseInstalled ? ["japanese", "mixed", "auto", "direct"] : ["auto", "direct"];
  const modeOn = { japanese, mixed, auto: true, direct: true };
  const switchable = { japanese: "japaneseEnabled", mixed: "mixedTypingEnabled" };
  const toggleKeys = [1, 2, 3].map((value) => [value, <Keycaps keys={switchKeyCaps(value, japaneseKeyboard, t)} />]).concat([[0, g.switchKey.none]]);
  const today = new Date();
  const pad = (value) => String(value).padStart(2, "0");
  const japaneseDate = `${today.getFullYear()}/${pad(today.getMonth() + 1)}/${pad(today.getDate())}`;
  const englishDate = today.toLocaleDateString("en-US", { month: "long", day: "numeric", year: "numeric" });
  const startModes = [[0, g.startMode.last]].concat(
    (japaneseInstalled ? [[1, "japanese"], [2, "mixed"]] : []).concat([[3, "auto"], [4, "direct"]])
      .filter(([, id]) => modeOn[id]).map(([value, id]) => [value, t.modeNames[id]]));
  return (
    <>
      <div className="mode-cards">
        {modes.map((id) => (
          <div key={id} role="button" tabIndex={0} aria-pressed={mode === id}
            className={`mode-card ${mode === id ? "is-current" : ""} ${modeOn[id] ? "" : "is-off"}`}
            onClick={() => modeOn[id] && changeMode(id)}
            onKeyDown={(event) => { if ((event.key === "Enter" || event.key === " ") && modeOn[id]) { event.preventDefault(); changeMode(id); } }}>
            <span className="mode-card-icon"><ModeIcon id={id} size={32} /></span>
            <span className="mode-card-name">
              {t.modeNames[id]}
              {switchable[id] && (
                <span onClick={(event) => event.stopPropagation()} onKeyDown={(event) => event.stopPropagation()}>
                  <Toggle label={t.modeNames[id]} checked={modeOn[id]} onChange={(value) => setSetting(switchable[id], value)} />
                </span>
              )}
            </span>
            <small>{mode === id ? t.inUse : ""}</small>
          </div>
        ))}
      </div>

      <Card label={g.switching}>
        <Row icon="keyboard" title={g.switchKey.title}>
          <Segmented label={g.switchKey.title} value={settings.toggleKey} options={toggleKeys} onChange={(value) => setSetting("toggleKey", value)} />
        </Row>
        {(japanese || mixed) && <SwitchOrder settings={settings} setSetting={setSetting} modeOn={modeOn} />}
      </Card>

      <Card label={g.indicator.title}>
        <Row icon="tooltip" title={g.indicator.title} description={g.indicator.description}
          sub={settings.modeIndicatorEnabled && (
            <Check label={g.indicator.onFocus} checked={settings.modeBadgeOnFocus} onChange={(value) => setSetting("modeBadgeOnFocus", value)} />
          )}>
          <Toggle label={g.indicator.title} checked={settings.modeIndicatorEnabled} onChange={(value) => setSetting("modeIndicatorEnabled", value)} />
        </Row>
      </Card>

      <Heading>{g.candidates}</Heading>
      <div className="looks" role="radiogroup" aria-label={g.candidates}>
        {["glass", "simple"].map((id, value) => (
          <button key={id} type="button" role="radio" aria-checked={settings.candidateWindowStyle === value} className="look"
            onClick={() => setSetting("candidateWindowStyle", value)}>
            <picture>
              <source media="(prefers-color-scheme: dark)" srcSet={`./assets/candidates/${id}-dark.png`} />
              <img src={`./assets/candidates/${id}-light.png`} alt="" />
            </picture>
            {g.styles[id]}
          </button>
        ))}
      </div>
      <Card label={g.candidates}>
        <Row icon="format_list_numbered" title={g.rows.title} description={g.rows.description}>
          <Segmented label={g.rows.title} value={settings.candidateRows} options={[[0, g.rows.automatic], [5, "5"], [7, "7"], [9, "9"]]}
            onChange={(value) => setSetting("candidateRows", value)} />
        </Row>
        <Row icon="menu_book" title={g.meanings.title} description={g.meanings.description}>
          <Toggle label={g.meanings.title} checked={settings.meaningsEnabled} onChange={(value) => setSetting("meaningsEnabled", value)} />
        </Row>
        {settings.meaningsEnabled && japaneseInstalled && (
          <Row icon="translate" title={g.meaningsLanguage.title} description={g.meaningsLanguage.description}>
            <Toggle label={g.meaningsLanguage.title} checked={settings.meaningsInUiLanguage} onChange={(value) => setSetting("meaningsInUiLanguage", value)} />
          </Row>
        )}
      </Card>

      <Details id="general" title={t.details}>
        <Card label={g.switching}>
          <Row icon="power_settings_new" title={g.startMode.title}>
            <Select label={g.startMode.title} value={startModes.some(([value]) => value === settings.startMode) ? settings.startMode : 0}
              options={startModes} onChange={(value) => setSetting("startMode", value)} />
          </Row>
          <Row icon="apps" title={g.modePerApp.title} description={g.modePerApp.description}>
            <Toggle label={g.modePerApp.title} checked={settings.modePerApp} onChange={(value) => setSetting("modePerApp", value)} />
          </Row>
          <Row icon="format_underlined" title={g.underline.title} description={g.underline.description}>
            <Toggle label={g.underline.title} checked={settings.modeUnderlineColors} onChange={(value) => setSetting("modeUnderlineColors", value)} />
          </Row>
          <Row icon="keyboard_alt" title={g.keyboard.title}>
            <Segmented label={g.keyboard.title} value={settings.keyboardType}
              options={[[0, g.keyboard.detect], [1, g.keyboard.japanese], [2, g.keyboard.us]]} onChange={(value) => setSetting("keyboardType", value)} />
          </Row>
        </Card>
        <Card label={g.extra}>
          <Row icon="calendar_today" title={g.date.title} description={g.date.description(japaneseDate, englishDate)}>
            <Toggle label={g.date.title} checked={settings.dateConversion} onChange={(value) => setSetting("dateConversion", value)} />
          </Row>
          <Row icon="emoji_symbols" title={g.symbols.title} description={g.symbols.description}>
            <Toggle label={g.symbols.title} checked={settings.symbolConversion} onChange={(value) => setSetting("symbolConversion", value)} />
          </Row>
          <Row icon="calculate" title={g.calculator.title} description={g.calculator.description}>
            <Toggle label={g.calculator.title} checked={settings.calculatorEnabled} onChange={(value) => setSetting("calculatorEnabled", value)} />
          </Row>
          <Row icon="list_alt" title={g.candidateList.title}>
            <Toggle label={g.candidateList.title} checked={settings.candidateWindowEnabled} onChange={(value) => setSetting("candidateWindowEnabled", value)} />
          </Row>
        </Card>
        <Card label={g.general}>
          <Row icon="data_object" title={g.brackets.title} description={g.brackets.description}>
            <Toggle label={g.brackets.title} checked={settings.bracketPairs} onChange={(value) => setSetting("bracketPairs", value)} />
          </Row>
          <Row icon="language" title={g.language.title}>
            <Segmented label={g.language.title} value={settings.uiLanguage} options={[[0, g.language.system], [1, "English"], [2, "日本語"]]}
              onChange={(value) => setSetting("uiLanguage", value)} />
          </Row>
        </Card>
        <ExcludedApps settings={settings} runAction={runAction} />
        <KeysCard page="general" showKeys={showKeys} />
      </Details>
    </>
  );
}

// The switch key's round: the modes, each on or off, dragged into order.
function SwitchOrder({ settings, setSetting, modeOn }) {
  const t = useText();
  const g = t.general;
  const ids = orderIds(settings.switchOrder).filter((id) => modeOn[id]);
  const [dragging, setDragging] = useState(null);
  const [target, setTarget] = useState(null);
  const inRound = (id) => (settings.switchModes & modeBits[id]) !== 0;
  const round = ids.filter(inRound);
  // Modes that are off keep their place in the order.
  const move = (id, to) => {
    const all = orderIds(settings.switchOrder);
    const shown = all.filter((other) => modeOn[other]);
    const from = shown.indexOf(id);
    if (from < 0 || to < 0 || to >= shown.length || from === to) return;
    const moved = [...shown];
    moved.splice(from, 1);
    moved.splice(to, 0, id);
    let next = 0;
    setSetting("switchOrder", orderValue(all.map((other) => (modeOn[other] ? moved[next++] : other))));
  };
  return (
    <Row icon="swap_horiz" title={g.order.title}
      sub={(
        <ul className="order" aria-label={g.order.title}>
          {ids.map((id, index) => (
            <li key={id} draggable className={`${inRound(id) ? "" : "is-off"} ${dragging === id ? "is-dragging" : ""} ${target === id ? "is-target" : ""}`}
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
              <input type="checkbox" aria-label={t.modeNames[id]} checked={inRound(id)}
                onChange={(event) => setSetting("switchModes", event.target.checked ? settings.switchModes | modeBits[id] : settings.switchModes & ~modeBits[id])} />
              <ModeIcon id={id} size={20} />
              <span className="name">{t.modeNames[id]}</span>
              <button type="button" className="icon-btn" aria-label={g.order.moveUp(t.modeNames[id])} disabled={index === 0} onClick={() => move(id, index - 1)}>
                <Symbol name="keyboard_arrow_up" size={18} />
              </button>
              <button type="button" className="icon-btn" aria-label={g.order.moveDown(t.modeNames[id])} disabled={index === ids.length - 1} onClick={() => move(id, index + 1)}>
                <Symbol name="keyboard_arrow_down" size={18} />
              </button>
            </li>
          ))}
        </ul>
      )}>
      <span className="cycle" aria-hidden="true">
        {round.map((id) => <React.Fragment key={id}><ModeIcon id={id} size={22} /><Symbol name="chevron_right" size={16} /></React.Fragment>)}
        <Symbol name="repeat" size={18} />
      </span>
    </Row>
  );
}

function ExcludedApps({ settings, runAction }) {
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
    <Card label={e.title}>
      <Row icon="block" title={e.title} description={e.description}
        sub={(
          <>
            <ul className="list" aria-label={e.title}>
              {settings.excludedApps.map((app) => (
                <li key={app}>
                  <span className="text">{app}</span>
                  <button type="button" className="icon-btn" aria-label={e.remove(app)} onClick={() => runAction("excludedApps.remove", { name: app })}>
                    <Symbol name="close" size={18} />
                  </button>
                </li>
              ))}
            </ul>
            <form className="inline-form" onSubmit={add}>
              <input aria-label={e.placeholder} placeholder={e.placeholder} value={name} onChange={(event) => setName(event.target.value)} />
              <Button type="submit" disabled={!name.trim()}>{t.add}</Button>
            </form>
          </>
        )}>
        <Button onClick={() => runAction("excludedApps.browse", {})}>{e.choose}</Button>
      </Row>
    </Card>
  );
}

// ---------------------------------------------------------------------------
// 日本語

// The kanji slider's stops and the settings each stands for.
const kanjiStops = [1, 2, 3, 4, 5, 6, 7, 8];
function kanjiStop(settings) {
  if (settings.japaneseKanjiRange === 3) return Math.min(Math.max(settings.japaneseKanjiGrade, 1), 6);
  if (settings.japaneseKanjiRange === 0) return 8;
  return 7;
}

function JapanesePage({ settings, setSetting, runAction, setConfirm, showKeys, romaji, postalCodes }) {
  const t = useText();
  const j = t.japanese;
  const [romajiOpen, setRomajiOpen] = useState(false);
  if (romajiOpen) return <RomajiTable romaji={romaji} runAction={runAction} setConfirm={setConfirm} back={() => setRomajiOpen(false)} />;
  const stop = kanjiStop(settings);
  const setKanji = async (value) => {
    if (value <= 6) {
      await setSetting("japaneseKanjiRange", 3);
      await setSetting("japaneseKanjiGrade", value);
    } else {
      await setSetting("japaneseKanjiRange", value === 7 ? 2 : 0);
    }
  };
  // Numbers: the style, with the matching width for the digits typed.
  const setNumbers = async (value) => {
    await setSetting("japaneseNumberStyle", value);
    if (value <= 3) await setSetting("japaneseDigitWidth", value - 1);
  };
  const numberOptions = j.numbers.options.map(([name, example], index) => [index + 1, name, example]);
  return (
    <>
      <Card label={j.live.title}>
        <Row icon="bolt" title={j.live.title} description={j.live.description}
          sub={settings.japaneseLiveConversion && (
            <Check label={j.live.refine} checked={settings.japaneseLiveRefine} onChange={(value) => setSetting("japaneseLiveRefine", value)} />
          )}>
          <Toggle label={j.live.title} checked={settings.japaneseLiveConversion} onChange={(value) => setSetting("japaneseLiveConversion", value)} />
        </Row>
        <Row icon="lightbulb" title={j.prediction.title} description={j.prediction.description}>
          <Toggle label={j.prediction.title} checked={settings.japanesePredictionEnabled} onChange={(value) => setSetting("japanesePredictionEnabled", value)} />
        </Row>
        <NumbersRow settings={settings} setSetting={setSetting} options={numberOptions} onChange={setNumbers} />
        <Row icon={<span className="row-glyph" aria-hidden="true">漢</span>} title={j.kanji.title} description={j.kanji.description}
          sub={stop < 8 && (
            <Check label={j.kanji.names} example={j.kanji.namesExample} checked={settings.japaneseKanjiNames}
              onChange={(value) => setSetting("japaneseKanjiNames", value)} />
          )}>
          <span style={{ display: "grid", gap: 4 }}>
            <Slider label={j.kanji.title} value={stop} stops={kanjiStops.map((value, index) => [value, j.kanji.stops[index]])} onChange={setKanji} />
            <small className="muted">{j.kanji.examples[stop <= 6 ? 0 : stop === 7 ? 1 : 2]}</small>
          </span>
        </Row>
      </Card>
      <Card label={j.romaji.title}>
        <LinkRow icon="table" title={j.romaji.title} description={j.romaji.description} onClick={() => setRomajiOpen(true)} />
        <PostalCodes postalCodes={postalCodes} runAction={runAction} setConfirm={setConfirm} />
      </Card>

      <Details id="japanese" title={t.details}>
        <Card label={j.input}>
          <Row icon="keyboard" title={j.kanaInput.title} description={j.kanaInput.description}
            sub={<Check label={j.kanaInput.key} checked={settings.japaneseKanaInputKey} onChange={(value) => setSetting("japaneseKanaInputKey", value)} />}>
            <Toggle label={j.kanaInput.title} checked={settings.japaneseKanaInput} onChange={(value) => setSetting("japaneseKanaInput", value)} />
          </Row>
          <Row icon="keyboard_capslock" title={j.capsLock.title}>
            <Toggle label={j.capsLock.title} checked={settings.japaneseIgnoreCapsLock} onChange={(value) => setSetting("japaneseIgnoreCapsLock", value)} />
          </Row>
        </Card>
        <Card label={j.notation}>
          <Row icon="edit_note" title={j.punctuation.title}>
            <Segmented label={j.punctuation.title} value={settings.japanesePunctuation} options={[[0, "、。"], [1, "，．"], [2, "，。"], [3, "、．"]]}
              onChange={(value) => setSetting("japanesePunctuation", value)} />
          </Row>
          <Row icon="space_bar" title={j.space.title}>
            <Segmented label={j.space.title} value={settings.japaneseSpaceWidth === 1 ? 1 : 0} options={[[1, j.space.half], [0, j.space.full]]}
              onChange={(value) => setSetting("japaneseSpaceWidth", value)} />
          </Row>
          <Row icon="abc" title={j.letters.title} description={j.letters.description}>
            <Segmented label={j.letters.title} value={settings.japaneseLetterWidth} options={[[1, j.space.half], [0, j.space.full]]}
              onChange={(value) => setSetting("japaneseLetterWidth", value)} />
          </Row>
          <Row icon="emoji_symbols" title={j.symbols.title} description={j.symbols.description}>
            <Segmented label={j.symbols.title} value={settings.japaneseSymbolWidth} options={[[1, j.space.half], [0, j.space.full]]}
              onChange={(value) => setSetting("japaneseSymbolWidth", value)} />
          </Row>
          <Row icon="format_letter_spacing" title={j.spacing.title} description={j.spacing.description}>
            <Toggle label={j.spacing.title} checked={settings.japaneseEnglishSpacing} onChange={(value) => setSetting("japaneseEnglishSpacing", value)} />
          </Row>
        </Card>
        <Card label={j.candidates}>
          <Row icon="notes" title={j.sentenceCheck.title} description={j.sentenceCheck.description}>
            <Toggle label={j.sentenceCheck.title} checked={settings.japaneseSentenceCheck} onChange={(value) => setSetting("japaneseSentenceCheck", value)} />
          </Row>
          <Row icon="mood" title={j.firstPageSymbol.title} description={j.firstPageSymbol.description}>
            <Toggle label={j.firstPageSymbol.title} checked={settings.japaneseFirstPageSymbol} onChange={(value) => setSetting("japaneseFirstPageSymbol", value)} />
          </Row>
          <Row icon="text_fields" title={j.typedForms.title} description={j.typedForms.description}>
            <Toggle label={j.typedForms.title} checked={settings.japaneseTypedForms} onChange={(value) => setSetting("japaneseTypedForms", value)} />
          </Row>
        </Card>
        <KeysCard page="japanese" showKeys={showKeys} />
      </Details>
    </>
  );
}

// 数字の変換: the style, and the checks that belong to numbers.
function NumbersRow({ settings, setSetting, options, onChange }) {
  const t = useText();
  const j = t.japanese;
  const [open, setOpen] = useState(false);
  const style = Math.min(Math.max(settings.japaneseNumberStyle, 1), 5);
  const current = options[style - 1];
  return (
    <div className="row-block">
      <button type="button" className="row is-link" aria-expanded={open} onClick={() => setOpen(!open)}>
        <span className="row-icon"><Symbol name="pin" /></span>
        <span className="row-text"><span className="row-title">{j.numbers.title}</span><small>{j.numbers.description}</small></span>
        <span className="row-value">{current[1]}<small>{current[2]}</small></span>
        <Symbol name={open ? "keyboard_arrow_up" : "keyboard_arrow_down"} size={18} className="row-chevron" />
      </button>
      {open && (
        <div className="row-panel" role="radiogroup" aria-label={j.numbers.title}>
          {options.map(([value, name, example]) => (
            <label key={value} className="radio">
              <input type="radio" name="numbers" checked={value === style} onChange={() => onChange(value)} />
              <span className="radio-name">{name}</span>
              <small>{example}</small>
            </label>
          ))}
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

// Postal codes to addresses: Japan Post's data, fetched only once the user
// agrees to it, or taken from a file they downloaded.
function PostalCodes({ postalCodes, runAction, setConfirm }) {
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
          {busy && <span className="row-status"><span className="progress" />{p.downloading}</span>}
        </>
      )}>
      <Toggle label={p.title} checked={postalCodes.installed || busy} disabled={busy} onChange={turn} />
    </Row>
  );
}

function RomajiTable({ romaji, runAction, setConfirm, back }) {
  const t = useText();
  const r = t.japanese.romaji;
  const [keys, setKeys] = useState("");
  const [kana, setKana] = useState("");
  const [unused, setUnused] = useState(false);
  const add = async (event) => {
    event.preventDefault();
    if (!keys.trim() || (!unused && !kana)) return;
    const response = await runAction("romaji.save", { input: keys.trim(), output: unused ? "" : kana, pending: "" }, r.saved);
    if (response.ok) {
      setKeys("");
      setKana("");
      setUnused(false);
    }
  };
  return (
    <>
      <Button icon="chevron_left" variant="quiet" className="back" onClick={back}>{r.back}</Button>
      <Card label={r.title}>
        <Row icon="table" title={r.title} description={r.description}
          sub={(
            <>
              <form className="inline-form" onSubmit={add}>
                <input aria-label={r.keys} placeholder={r.keys} value={keys} onChange={(event) => setKeys(event.target.value)} />
                <input aria-label={r.kana} placeholder={r.kana} value={kana} disabled={unused} onChange={(event) => setKana(event.target.value)} />
                <label><input type="checkbox" checked={unused} onChange={(event) => setUnused(event.target.checked)} />{r.unused}</label>
                <Button variant="primary" type="submit">{t.add}</Button>
              </form>
              <ul className="list" aria-label={r.title}>
                {romaji.map((row) => (
                  <li key={row.input}>
                    <span className="reading">{row.input}</span>
                    <span className="text">{row.output || r.unused}</span>
                    {row.standard && <span className="tag">{r.standard(row.standard)}</span>}
                    <button type="button" className="icon-btn" aria-label={r.removeLabel(row.input)}
                      onClick={() => runAction("romaji.delete", { input: row.input }, r.removed)}>
                      <Symbol name="close" size={18} />
                    </button>
                  </li>
                ))}
              </ul>
              {romaji.length === 0 && <p className="list-empty">{r.empty}</p>}
            </>
          )}>
          <Button onClick={() => runAction("romaji.browse", {}, (response) => response.text)}>{r.import}</Button>
          <Button disabled={romaji.length === 0} onClick={() => setConfirm({
            title: r.resetTitle, message: r.resetMessage, confirmLabel: r.reset, danger: true,
            action: () => runAction("romaji.reset", {}, r.wasReset),
          })}>{r.reset}</Button>
        </Row>
      </Card>
    </>
  );
}

// ---------------------------------------------------------------------------
// まぜ打ち

function MixedPage({ settings, setSetting }) {
  const t = useText();
  const m = t.mixed;
  return (
    <>
      <Card label={t.pages.mixed}>
        <Row icon="bolt" title={m.live.title} description={m.live.description}>
          <Toggle label={m.live.title} checked={settings.mixLiveConversion} onChange={(value) => setSetting("mixLiveConversion", value)} />
        </Row>
        <Row icon="lightbulb" title={m.prediction.title}>
          <Toggle label={m.prediction.title} checked={settings.mixPredictionEnabled} onChange={(value) => setSetting("mixPredictionEnabled", value)} />
        </Row>
        <Row icon="text_format" title={m.singleVowel.title} description={m.singleVowel.description}>
          <Segmented label={m.singleVowel.title} value={settings.mixSingleVowelKana} options={[[true, m.singleVowel.kana], [false, m.singleVowel.letter]]}
            onChange={(value) => setSetting("mixSingleVowelKana", value)} />
        </Row>
        <Row icon="spellcheck" title={m.correction.title} description={m.correction.description}>
          <Toggle label={m.correction.title} checked={settings.mixCorrection} onChange={(value) => setSetting("mixCorrection", value)} />
        </Row>
      </Card>
      <Details id="mixed" title={t.details}>
        <Card label={m.firstPageSymbol.title}>
          <Row icon="mood" title={m.firstPageSymbol.title} description={m.firstPageSymbol.description}>
            <Toggle label={m.firstPageSymbol.title} checked={settings.mixFirstPageSymbol} onChange={(value) => setSetting("mixFirstPageSymbol", value)} />
          </Row>
        </Card>
        <Card label={m.shared.title}>
          <Row icon="tune" title={m.shared.title} description={m.shared.description} />
        </Card>
      </Details>
    </>
  );
}

// ---------------------------------------------------------------------------
// Auto

function AutoPage({ settings, setSetting, showKeys }) {
  const t = useText();
  const a = t.auto;
  return (
    <>
      <Card label={a.corrections}>
        <Row icon="spellcheck" title={a.correction.title} description={a.correction.description}>
          <Toggle label={a.correction.title} checked={settings.correctionEnabled} onChange={(value) => setSetting("correctionEnabled", value)} />
        </Row>
        <Row icon="flag" title={a.spelling.title} description={a.spelling.description}>
          <Segmented label={a.spelling.title} value={settings.englishSpelling} options={a.spelling.options.map((name, value) => [value, name])}
            onChange={(value) => setSetting("englishSpelling", value)} />
        </Row>
      </Card>
      <Card label={a.candidates}>
        <Row icon="short_text" title={a.completion.title} description={a.completion.description}>
          <Toggle label={a.completion.title} checked={settings.completionEnabled} onChange={(value) => setSetting("completionEnabled", value)} />
        </Row>
        <Row icon="text_select_end" title={a.next.title} description={a.next.description}>
          <Toggle label={a.next.title} checked={settings.nextWordPrediction} onChange={(value) => setSetting("nextWordPrediction", value)} />
        </Row>
        <Row icon="forum" title={a.casual.title} description={a.casual.description}>
          <Slider label={a.casual.title} value={settings.socialExpressionRange} stops={a.casual.stops.map((name, value) => [value, name])}
            onChange={(value) => setSetting("socialExpressionRange", value)} />
        </Row>
      </Card>
      <Details id="auto" title={t.details}>
        <Card label={a.corrections}>
          <Row icon="repeat" title={a.doubled.title} description={a.doubled.description}>
            <Toggle label={a.doubled.title} checked={settings.doubledWordCheck} onChange={(value) => setSetting("doubledWordCheck", value)} />
          </Row>
          <Row icon="undo" title={a.recheck.title} description={a.recheck.description}>
            <Toggle label={a.recheck.title} checked={settings.recheckPreviousWord} onChange={(value) => setSetting("recheckPreviousWord", value)} />
          </Row>
        </Card>
        <Card label={a.candidates}>
          <Row icon="record_voice_over" title={a.phonetic.title} description={a.phonetic.description}>
            <Toggle label={a.phonetic.title} checked={settings.japanesePhoneticSuggestionsEnabled}
              onChange={(value) => setSetting("japanesePhoneticSuggestionsEnabled", value)} />
          </Row>
        </Card>
        <Card label={a.punctuation}>
          <Row icon="keyboard_return" title={a.period.title} description={a.period.description}>
            <Toggle label={a.period.title} checked={settings.periodOnEnter} onChange={(value) => setSetting("periodOnEnter", value)} />
          </Row>
          <Row icon="space_bar" title={a.doubleSpace.title} description={a.doubleSpace.description}>
            <Toggle label={a.doubleSpace.title} checked={settings.doubleSpacePeriod} onChange={(value) => setSetting("doubleSpacePeriod", value)} />
          </Row>
          <Row icon="format_quote" title={a.smart.title} description={a.smart.description}>
            <Toggle label={a.smart.title} checked={settings.smartPunctuation} onChange={(value) => setSetting("smartPunctuation", value)} />
          </Row>
        </Card>
        <KeysCard page="auto" showKeys={showKeys} />
      </Details>
    </>
  );
}

// ---------------------------------------------------------------------------
// 辞書と学習

function DictionaryPage({ settings, setSetting, runAction, setConfirm, japanese, learning, dictionary, japaneseWords, setModal }) {
  const t = useText();
  const d = t.dictionary;
  const l = t.learningList;
  const [query, setQuery] = useState("");
  const filtered = useMemo(() => {
    const needle = query.trim().toLowerCase();
    return dictionary.filter((entry) => !needle || `${entry.raw} ${entry.candidate}`.toLowerCase().includes(needle));
  }, [dictionary, query]);
  const [learned, setLearned] = useState(null);
  const [learnedQuery, setLearnedQuery] = useState("");
  const showLearned = async () => {
    const response = await runAction("learning.list", {});
    if (response.ok) setLearned(response.items || []);
  };
  const forgetLearned = async (item) => {
    const response = await runAction("learning.forget", { kind: item.kind, key: item.key, value: item.value }, l.forgotten);
    if (response.ok) setLearned(response.items || []);
  };
  const shownLearned = useMemo(() => {
    if (!learned) return [];
    const needle = learnedQuery.trim().toLowerCase();
    return (needle ? learned.filter((item) => `${item.key} ${item.value}`.toLowerCase().includes(needle)) : learned).slice(0, 200);
  }, [learned, learnedQuery]);
  // One switch for everything learned from choices.
  const setLearningEnabled = async (value) => {
    if (await setSetting("learningEnabled", value)) {
      if (Boolean(settings.socialPersonalization) !== value) await setSetting("socialPersonalization", value ? 1 : 0);
    }
  };
  const askClear = () => setConfirm({
    title: l.confirmTitle, message: l.confirmMessage, confirmLabel: d.learned.clear, danger: true,
    action: async () => {
      await runAction("learning.clear", {});
      if (japanese) await runAction("japaneseLearning.clear", {});
      setLearned(null);
    },
  });
  const askDelete = (title, message, action) => setConfirm({ title, message, confirmLabel: t.remove, danger: true, action });
  return (
    <>
      <Heading>{d.words}</Heading>
      <Card label={d.words}>
        {japanese && (
          <Row icon="book" title={d.japaneseWords.title} description={d.japaneseWords.description}
            sub={(
              <>
                <ul className="list" aria-label={d.japaneseWords.title}>
                  {japaneseWords.map((word) => (
                    <li key={`${word.reading}\u0001${word.surface}`}>
                      <span className="reading">{word.reading}</span>
                      <button type="button" className="text" style={{ border: 0, background: "none", textAlign: "left", padding: 0 }}
                        onClick={() => setModal({ japanese: true, mode: "edit", word })}>{word.surface}</button>
                      <span className="tag">{word.action === "suppress" ? t.japaneseWords.actions.suppress.label : t.japaneseWords.kinds[word.kind] || word.kind}</span>
                      <button type="button" className="icon-btn" aria-label={t.words.removeLabel(word.surface)}
                        onClick={() => askDelete(t.japaneseWords.removeTitle, t.japaneseWords.removeMessage(word.surface),
                          () => runAction("japaneseWords.delete", { reading: word.reading, surface: word.surface }, t.japaneseWords.removed))}>
                        <Symbol name="close" size={18} />
                      </button>
                    </li>
                  ))}
                </ul>
                {japaneseWords.length === 0 && <p className="list-empty">{t.japaneseWords.empty}</p>}
              </>
            )}>
            <Button variant="primary" onClick={() => setModal({ japanese: true, mode: "new", word: {} })}>{t.add}</Button>
          </Row>
        )}
        <Row icon="book" title={d.englishWords.title} description={d.englishWords.description}
          sub={(
            <>
              {dictionary.length > 5 && (
                <div className="list-search">
                  <input aria-label={t.words.search} placeholder={t.words.search} value={query} onChange={(event) => setQuery(event.target.value)} />
                </div>
              )}
              <ul className="list" aria-label={d.englishWords.title}>
                {filtered.map((entry) => {
                  const action = actionOf(entry.policy);
                  return (
                    <li key={entry.id}>
                      <button type="button" className="reading" style={{ border: 0, background: "none", textAlign: "left", padding: 0 }}
                        onClick={() => setModal({ mode: "edit", entry })}>{entry.raw}</button>
                      <span className="text">{action === "keep" ? "" : entry.candidate}</span>
                      <span className="tag">{t.actions[action].label}</span>
                      <button type="button" className="icon-btn" aria-label={t.words.removeLabel(entry.raw)}
                        onClick={() => askDelete(t.words.removeTitle, t.words.removeMessage(entry.raw),
                          () => runAction("dictionary.delete", { id: entry.id }, t.words.removed))}>
                        <Symbol name="close" size={18} />
                      </button>
                    </li>
                  );
                })}
              </ul>
              {filtered.length === 0 && <p className="list-empty">{dictionary.length === 0 ? t.words.empty : t.words.noMatch}</p>}
            </>
          )}>
          <Button variant="primary" onClick={() => setModal({ mode: "new", entry: {} })}>{t.add}</Button>
        </Row>
        <Row icon="upload_file" title={d.files.title} description={japanese ? d.files.description : d.files.descriptionEnglish}>
          <Button onClick={() => runAction("japaneseWords.browse", {}, (response) => response.text)}>{d.files.import}</Button>
          <Button onClick={() => runAction("dictionary.save", {}, (response) => (response.saved ? d.files.exported : ""))}>{d.files.export}</Button>
        </Row>
      </Card>

      <Heading>{d.learning}</Heading>
      <Card label={d.learning}>
        <Row icon="school" title={d.learn.title} description={d.learn.description}>
          <Toggle label={d.learn.title} checked={learning.enabled} onChange={setLearningEnabled} />
        </Row>
        <Row icon="history" title={d.learned.title} description={d.learned.count(learning.count || 0)}
          sub={learned && (
            <>
              {learned.length > 5 && (
                <div className="list-search">
                  <input aria-label={l.search} placeholder={l.search} value={learnedQuery} onChange={(event) => setLearnedQuery(event.target.value)} />
                </div>
              )}
              <ul className="list" aria-label={l.title}>
                {shownLearned.map((item) => (
                  <li key={`${item.kind}\u0000${item.key}\u0000${item.value}`}>
                    <span className="reading">{item.key}</span>
                    <span className="text">{item.value}</span>
                    <span className="tag">{l.how[item.how](item.count)}</span>
                    <button type="button" className="icon-btn" aria-label={l.forgetLabel(item.value || item.key)} onClick={() => forgetLearned(item)}>
                      <Symbol name="close" size={18} />
                    </button>
                  </li>
                ))}
              </ul>
              {shownLearned.length === 0 && <p className="list-empty">{learned.length === 0 ? l.nothing : t.words.noMatch}</p>}
              {learned.length > shownLearned.length && !learnedQuery && <p className="list-empty">{l.more(learned.length - shownLearned.length)}</p>}
            </>
          )}>
          <Button onClick={learned ? () => setLearned(null) : showLearned}>{learned ? t.close : t.list}</Button>
          <Button onClick={askClear}>{d.learned.clear}</Button>
        </Row>
        <Row icon="lock" title={d.private.title} description={d.private.description}>
          <Toggle label={d.private.title} checked={settings.noLearningInPrivate} onChange={(value) => setSetting("noLearningInPrivate", value)} />
        </Row>
      </Card>
    </>
  );
}

// ---------------------------------------------------------------------------
// 情報

function AboutPage({ settings, setSetting, runAction, version, runtime, packs, japanese }) {
  const t = useText();
  const a = t.about;
  const [license, setLicense] = useState(null);
  const openLicense = async () => {
    const response = await hostRequest("license.get");
    setLicense(response.ok ? response.text : a.licenseMissing);
  };
  // The third-party notices and full license texts carried in the page
  // (copy-notices.mjs); `plain` for a text file.
  const [notice, setNotice] = useState(null);
  const openNotice = async (file, title, plain = false) => {
    let text = a.licenseMissing;
    try {
      const response = await fetch(`./notices/${file}`);
      if (response.ok) text = await response.text();
    } catch {
      // Shown as missing.
    }
    setNotice({ title, text, plain });
  };
  // Problems only: a pack of an installed language that cannot be used, or
  // the input method not registered.
  const installedGroups = new Set(packs.filter((pack) => pack.valid).map((pack) => pack.group || "english"));
  const problems = [
    ...(runtime.tsf !== "Loaded" ? [a.tsfProblem] : []),
    ...packs.filter((pack) => !pack.valid && installedGroups.has(pack.group || "english")).map((pack) => a.packProblem(pack.displayName)),
  ];
  const [recent, setRecent] = useState(null);
  const listRecent = async () => {
    const response = await runAction("recent.list", {});
    if (response.ok) setRecent(response.items || []);
  };
  const forgetRecent = async (id) => {
    const response = await runAction("recent.forget", { id });
    if (response.ok) setRecent(response.items || []);
  };
  useEffect(() => {
    if (japanese && settings.keepRecentConversions) listRecent();
    // Listed once on opening; the host keeps it until asked again.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [japanese, settings.keepRecentConversions]);
  return (
    <>
      <Card label="TEKITO">
        <div className="about-head">
          <img src="./assets/tekito.ico" alt="" />
          <span><Wordmark /><small>{a.version(version)}</small></span>
        </div>
      </Card>
      {problems.length > 0 && (
        <Card label={a.problemTitle}>
          {problems.map((problem) => <Row key={problem} icon="info" title={<span className="problem">{problem}</span>} />)}
        </Card>
      )}

      <Heading>{a.feedback}</Heading>
      <Card label={a.feedback}>
        {japanese && (
          <Row icon="feedback" title={a.report.title} description={a.report.description}
            sub={settings.keepRecentConversions && recent && (
              <>
                <ul className="list" aria-label={a.report.title}>
                  {recent.map((item) => (
                    <li key={item.id}>
                      <span className="reading">{item.reading}</span>
                      <span className="text">{item.text}</span>
                      {item.mode === "Mix" && <span className="tag">{t.modeNames.mixed}</span>}
                      <span className="actions">
                        <Button onClick={() => runAction("recent.report", { id: item.id }, a.report.opened)}>{a.report.report}</Button>
                        <button type="button" className="icon-btn" aria-label={t.learningList.forgetLabel(item.text)} onClick={() => forgetRecent(item.id)}>
                          <Symbol name="close" size={18} />
                        </button>
                      </span>
                    </li>
                  ))}
                </ul>
                {recent.length === 0 && <p className="list-empty">{a.report.nothing}</p>}
              </>
            )}>
            <Toggle label={a.report.title} checked={settings.keepRecentConversions}
              onChange={(value) => { setSetting("keepRecentConversions", value); if (!value) setRecent(null); }} />
          </Row>
        )}
        <LinkRow icon="bug_report" title={a.bug.title} description={a.bug.description} external onClick={() => runAction("feedback.open", { kind: "bug" })} />
        <LinkRow icon="lightbulb" title={a.feature.title} description={a.feature.description} external onClick={() => runAction("feedback.open", { kind: "feature" })} />
      </Card>

      <Heading>{a.support}</Heading>
      <Card label={a.support}>
        <LinkRow icon="history" title={a.releases.title} description={a.releases.description} external onClick={() => runAction("feedback.open", { kind: "releases" })} />
        <LinkRow icon="content_copy" title={a.diagnostics} onClick={() => runAction("diagnostics.copy", {}, a.diagnosticsCopied)} />
        <LinkRow icon="folder" title={a.logs} onClick={() => runAction("logs.open", {})} />
        <LinkRow icon="gavel" title={a.license} onClick={openLicense} />
        <LinkRow icon="description" title={a.notices} onClick={() => openNotice("THIRD_PARTY_NOTICES.md", a.notices)} />
      </Card>

      <Heading>{a.publisher}</Heading>
      <Card label={a.publisher}>
        <div className="publisher"><CapitataLogo className="capitata" /></div>
        <LinkRow icon="language" title="capitata.dev" external onClick={() => runAction("website.open", {})} />
        <LinkRow icon="code" title="GitHub" description="github.com/Canta360/tekito" external onClick={() => runAction("feedback.open", { kind: "github" })} />
      </Card>
      <div className="footnote"><span>© 2026 Capitata</span></div>

      <Dialog open={license !== null} title={a.licenseTitle} wide onClose={() => setLicense(null)}>
        {a.licenseNote && <p className="dialog-message muted">{a.licenseNote}</p>}
        <LicenseText text={license || ""} />
        <div className="dialog-actions"><Button variant="primary" onClick={() => setLicense(null)}>{t.close}</Button></div>
      </Dialog>
      <Dialog open={notice !== null} title={notice?.title} wide onClose={() => setNotice(null)}>
        {notice && (notice.plain ? <pre className="license-text plain" tabIndex={0} lang="en">{notice.text}</pre> : <LicenseText text={notice.text} />)}
        {notice && !notice.plain && (
          <div className="notice-links">
            <Button onClick={() => openNotice("material-symbols-LICENSE.txt", "Material Symbols — Apache License 2.0", true)}>Material Symbols</Button>
            <Button onClick={() => openNotice("OFL.txt", "M PLUS 1 — SIL Open Font License 1.1", true)}>M PLUS 1</Button>
            <Button onClick={() => openNotice("webview2-LICENSE.txt", "Microsoft WebView2", true)}>WebView2</Button>
            <Button onClick={() => runAction("notices.open", {})}>{a.dataNotices}</Button>
          </div>
        )}
        <div className="dialog-actions"><Button variant="primary" onClick={() => setNotice(null)}>{t.close}</Button></div>
      </Dialog>
    </>
  );
}

// Renders the license Markdown's few constructs: headings, bullets, paragraphs.
function LicenseText({ text }) {
  const blocks = [];
  // A line that only continues the one before (wrapped text) joins it.
  let open = false;
  text.replace(/\r/g, "").split("\n").forEach((raw, index) => {
    // Markdown quotes and code marks read as plain text.
    const line = raw.trim().replace(/^>\s?/, "").replace(/`/g, "");
    const last = blocks[blocks.length - 1];
    if (!line || index === 0) {  // line 0 is the title, shown as the dialog heading
      open = false;
    } else if (line.startsWith("- ")) {
      if (last?.kind === "list") last.items.push(line.slice(2));
      else blocks.push({ kind: "list", items: [line.slice(2)] });
      open = true;
    } else if (line.startsWith("## ")) {
      blocks.push({ kind: "heading", text: line.slice(3) });
      open = false;
    } else if (open && last?.kind === "list") {
      last.items[last.items.length - 1] += ` ${line}`;
    } else if (open && last?.kind === "paragraph") {
      last.text += ` ${line}`;
    } else {
      blocks.push({ kind: "paragraph", text: line });
      open = true;
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
        <Button type="submit" variant="primary" disabled={!valid}>{t.save}</Button>
        <Button onClick={onCancel}>{t.cancel}</Button>
      </div>
    </form>
  );
}

// The kinds a Japanese word can be (JapaneseUserDictionary.h), in the order
// the form lists them, and what converting does with it.
const japaneseKinds = ["noun", "proper-noun", "person", "surname", "given-name", "place", "organization",
  "suru-noun", "symbol", "interjection"];
const japaneseActions = ["first", "suggest", "suppress"];
// A reading: hiragana, or katakana (the host makes it hiragana).
const readingPattern = /^[ぁ-ゖァ-ヶーゝゞ]+$/;

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
        <Button type="submit" variant="primary" disabled={!valid}>{t.save}</Button>
        <Button onClick={onCancel}>{t.cancel}</Button>
      </div>
    </form>
  );
}

const start = import.meta.env.DEV && !window.chrome?.webview
  ? import("./devHost.js").then((module) => module.installMockHost())
  : Promise.resolve();
start.then(() => createRoot(document.getElementById("root")).render(<App />));
