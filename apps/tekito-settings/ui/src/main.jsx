import React, { useCallback, useEffect, useId, useMemo, useRef, useState } from "react";
import {
  Checkbox, Field, FluentProvider, NavDrawer, NavDrawerBody, NavDrawerFooter, NavDrawerHeader, NavItem, Toast, Toaster, ToastTitle,
  useToastController,
} from "@fluentui/react-components";
import { createRoot } from "react-dom/client";
import { CapitataLogo } from "./CapitataLogo.jsx";
import { hostRequest, onHostStateChanged } from "./host.js";
import { messages, resolveLanguage, TextContext, useText } from "./i18n.js";
import {
  Button, Card, Details, Dialog, Heading, IconButton, Keycaps, LinkRow, ModeIcon, Row, Segmented, Symbol, TextBox, Toggle, Wordmark, TekitoSymbol,
  LineWordmark,
} from "./ui.jsx";
import { brandVariables, hueColor, tekitoTheme } from "./theme.js";
import { mascotRainbow } from "./mascot.jsx";
import { CatalogPage, footPages, navIcons, navPages, pageGroups } from "./catalog.jsx";
import { AllSettingsPage, HomePage } from "./home.jsx";
import { EncryptionRow, KeyList } from "./rows.jsx";
import "./styles.css";

// The pages, in the order of the list.
const pageIds = [...navPages, ...footPages];
// Pages opened from elsewhere (the list, or the setup's --page) that are
// not in the list.
const subPages = ["samples", "romaji"];
// Links from before the pages were rearranged.
const pageAliases = {
  japanese: "conversion", mixed: "conversion", ai: "conversion", auto: "slips", switching: "general", english: "slips",
  typing: "slips", special: "candidates",
};

// Mirrors the defaults in src/UserData/UserSettings.h; only shown until the
// host's first reply arrives, and what All settings resets to.
const initialSettings = {
  startMode: 0,
  switchOrder: 1234,
  switchModes: 7,
  correctionEnabled: true,
  completionEnabled: true,
  candidateWindowEnabled: true,
  learningEnabled: true,
  secretMode: false,
  japanesePhoneticSuggestionsEnabled: true,
  socialExpressionRange: 1,
  socialPersonalization: 1,
  candidateWindowStyle: 0,
  candidateRows: 0,
  aiSettingsControl: 1,
  mixPunctuation: 0,
  mixDigitWidth: 0,
  mixNumberStyle: 1,
  mixKanjiRange: 0,
  mixKanjiGrade: 6,
  mixSymbolWidth: 0,
  mixLetterWidth: 1,
  mixSpaceWidth: 0,
  mixNotationLevel: 3,
  mixRoughNumbersKanji: true,
  mixKanjiNames: false,
  mixKanjiVariants: true,
  mixSentenceCheck: true,
  mixSlipAutoApply: true,
  mixTypedForms: true,
  mixLiveRefine: true,
  mixEnglishSpacing: false,
  meaningsEnabled: true,
  meaningsInUiLanguage: true,
  mixPredictionEnabled: true,
  mixFirstPageSymbol: true,
  mixCorrection: true,
  spacelessEnglish: true,
  spacelessEnglishAlways: false,
  mixSettledMarker: true,
  mixEnglishUnderline: false,
  rainbowAccent: false,
  catSounds: false,
  desktopCompanion: false,
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
  capitalizedCorrection: false,
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
  japaneseSentenceModel: 0,
  japaneseAiDevice: 0,
  japaneseAiGpu: 0,
  japaneseSlipAutoApply: true,
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
  japaneseNumpadDirect: true,
  dateConversion: true,
  numberConversion: true,
  symbolConversion: true,
  calculatorEnabled: true,
  uiLanguage: 0,
  excludedApps: [],
  builtInExcludedApps: [],
};

function pageFromHash() {
  const id = window.location.hash.slice(1);
  return pageAliases[id] || (pageIds.includes(id) || subPages.includes(id) ? id : "home");
}

// The AI model files by japaneseSentenceModel.
const aiModelKeys = ["", "light", "strong"];

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
  const [encryption, setEncryption] = useState({ wanted: false, encrypted: false });
  const [aiModels, setAiModels] = useState({ light: { installed: false }, strong: { installed: false }, runtime: { installed: false },
    downloading: 0, error: "" });
  const [version, setVersion] = useState("");
  const [appearance, setAppearance] = useState({ accent: "#0067c0", systemLanguage: navigator.language.startsWith("ja") ? "ja" : "en" });
  const [modal, setModal] = useState(null);
  const [confirm, setConfirm] = useState(null);
  const [keys, setKeys] = useState(null);
  // The page list: whole, or folded to its icons by its TEKITO corner (remembered).
  const [navOpen, setNavOpen] = useState(() => {
    try {
      return window.localStorage.getItem("tekito.nav") !== "0";
    } catch {
      return true;
    }
  });
  // A narrow window folds it on its own (unfolded there by the corner, for
  // the moment only).
  const [narrow, setNarrow] = useState(() => Boolean(window.matchMedia?.("(max-width: 760px)").matches));
  const [narrowOpen, setNarrowOpen] = useState(false);
  useEffect(() => {
    const query = window.matchMedia?.("(max-width: 760px)");
    if (!query) return undefined;
    const changed = (event) => { setNarrow(event.matches); setNarrowOpen(false); };
    query.addEventListener("change", changed);
    return () => query.removeEventListener("change", changed);
  }, []);
  const listOpen = narrow ? narrowOpen : navOpen;
  const toggleNav = () => {
    if (narrow) {
      setNarrowOpen(!narrowOpen);
      return;
    }
    setNavOpen(!navOpen);
    try {
      window.localStorage.setItem("tekito.nav", navOpen ? "0" : "1");
    } catch {
      // Remembering is a convenience only.
    }
  };
  // Home's conversation with AI, kept while Settings is open.
  const [conversation, setConversation] = useState([]);

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
    if (state.aiModels) setAiModels(state.aiModels);
    if (state.encryption) setEncryption(state.encryption);
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

  // Fluent 2's theme in Windows' accent color, light or dark as Windows is.
  const [dark, setDark] = useState(() => Boolean(window.matchMedia?.("(prefers-color-scheme: dark)").matches));
  useEffect(() => {
    const query = window.matchMedia?.("(prefers-color-scheme: dark)");
    if (!query) return undefined;
    const changed = (event) => setDark(event.matches);
    query.addEventListener("change", changed);
    return () => query.removeEventListener("change", changed);
  }, []);
  const theme = useMemo(() => tekitoTheme(appearance.accent, dark), [appearance.accent, dark]);
  // Rainbow: the brand colors go round the hues, set on the page's root
  // (not the theme, so nothing is drawn again for it).
  const root = useRef(null);
  useEffect(() => {
    if (!settings.rainbowAccent) return undefined;
    const started = performance.now();
    let names = [];
    const timer = window.setInterval(() => {
      const element = root.current;
      if (!element) return;
      const hue = Math.round(((performance.now() - started) / 4000) * 360) % 360;
      // The companion (mascot.jsx) turns with it, from its own blue.
      const variables = [...brandVariables(hueColor(hue), dark), ...mascotRainbow(hue)];
      for (const [name, value] of variables) element.style.setProperty(name, value);
      names = variables.map(([name]) => name);
    }, 50);
    return () => {
      window.clearInterval(timer);
      for (const name of names) root.current?.style.removeProperty(name);
    };
  }, [dark, settings.rainbowAccent]);

  useEffect(() => {
    document.documentElement.lang = t.lang;
    document.title = `TEKITO ${t.settings}`;
  }, [t]);

  // Notices are Fluent's toasts.
  const toaster = useId();
  const { dispatchToast } = useToastController(toaster);
  const showNotice = (message, tone = "positive") => dispatchToast(<Toast><ToastTitle>{message}</ToastTitle></Toast>,
    { intent: tone === "negative" ? "error" : "success", timeout: 2400, position: "bottom" });

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

  const current = pageIds.includes(page) || subPages.includes(page) ? page : "home";
  // Each visit draws the page afresh, so one opened from search on the page
  // already shown opens its 詳細 too. A setting opened from Home is brought
  // into view and marked for a moment.
  const [visit, setVisit] = useState(0);
  const [focusRow, setFocusRow] = useState(null);
  const open = (id, rowId, details) => {
    if (details) {
      try {
        window.localStorage.setItem(`tekito.details.${details}`, "1");
      } catch {
        // Opening 詳細 for it is a convenience only.
      }
    }
    setPage(id);
    setVisit((n) => n + 1);
    setFocusRow(rowId || null);
    if (!rowId) document.querySelector(".content")?.scrollTo(0, 0);
  };
  useEffect(() => {
    if (!focusRow) return undefined;
    const timer = window.setTimeout(() => {
      const element = document.getElementById(`row-${focusRow}`);
      if (element) {
        element.scrollIntoView({ block: "center" });
        element.classList.add("is-found");
      }
      setFocusRow(null);
    }, 30);
    return () => window.clearTimeout(timer);
  }, [focusRow, visit]);
  // Ctrl+F: Home's box, from any page.
  useEffect(() => {
    const onKeyDown = (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "f") {
        event.preventDefault();
        setPage("home");
        setVisit((n) => n + 1);
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, []);

  // AI answers Home's box once a model is chosen and there.
  // Home asks AI once Settings' AI model is there; with a conversion model
  // and not it, Home offers to download it.
  const chosenModel = settings.japaneseSentenceModel || 0;
  const conversionAi = chosenModel > 0 && Boolean(aiModels[aiModelKeys[chosenModel]]?.installed) && Boolean(aiModels.runtime?.installed);
  const aiReady = Boolean(aiModels.settings?.installed) && Boolean(aiModels.runtime?.installed);
  const settingsAiOffer = conversionAi && !aiReady;
  const on = { japanese, mixed, auto: true, direct: true };
  const japaneseKeyboard = Boolean(runtime.japaneseKeyboard);
  const ctx = {
    t, settings, setSetting, runAction, setConfirm, showKeys: setKeys, open, on, multi: japanese || mixed, japaneseInstalled,
    japaneseKeyboard, aiModels, postalCodes, openRomaji: () => open("romaji"), aiReady, settingsAiOffer, chat: { messages: conversation, setMessages: setConversation },
    japaneseInstalling: Boolean(runtime.japaneseInstalling), japaneseDataBytes: runtime.japaneseDataBytes || 0,
    japaneseAddable: runtime.japaneseAddable !== false, japaneseDataError: runtime.japaneseDataError || "",
    encryption,
  };
  const pageProps = { settings, setSetting, runAction, setConfirm, showKeys: setKeys, open };

  return (
    <TextContext.Provider value={t}>
      <FluentProvider theme={theme} className="provider" ref={root}>
      <div className="app">
        <NavDrawer open type="inline" className={`sidebar ${listOpen ? "" : "is-rail"}`} aria-label={t.settings} selectedValue={current}
          onNavItemSelect={(event, data) => open(data.value)}>
          <NavDrawerHeader>
            <button type="button" className="brand" aria-expanded={listOpen} aria-label={t.menu} title={t.menu} onClick={toggleNav}>
              <TekitoSymbol size={20} />
              {listOpen && <><LineWordmark height={13} />{t.settings}</>}
            </button>
          </NavDrawerHeader>
          <NavDrawerBody>
            {navPages.map((id) => <PageItem key={id} id={id} rail={!listOpen} />)}
          </NavDrawerBody>
          <NavDrawerFooter>
            {footPages.map((id) => <PageItem key={id} id={id} rail={!listOpen} />)}
          </NavDrawerFooter>
        </NavDrawer>

        <main className="content">
          <div className="page" key={`${current}-${visit}`}>
            {/* Home is the box itself: its title only for screen readers. */}
            <h1 className={`page-title ${current === "home" ? "visually-hidden" : ""}`}>{t.pages[current]}</h1>
            {connection.status !== "ready" ? (
              <Card className="state-card" role="status">
                <b>{connection.status === "loading" ? t.loading.title : t.unavailable.title}</b>
                <span className="muted">{connection.status === "loading" ? t.loading.body : connection.error || t.unavailable.body}</span>
                {connection.status === "error" && <Button variant="primary" onClick={reload}>{t.tryAgain}</Button>}
              </Card>
            ) : (
              <>
                {current === "home" && <HomePage ctx={ctx} aiReady={aiReady} />}
                {pageGroups[current] && <CatalogPage page={current} ctx={ctx} />}
                {current === "all" && <AllSettingsPage ctx={ctx} defaults={initialSettings} />}
                {current === "dictionary" && <DictionaryPage {...pageProps} japanese={japaneseInstalled} learning={learning} encryption={encryption}
                  dictionary={dictionary} japaneseWords={japaneseWords} setModal={setModal} />}
                {current === "samples" && <SamplesPage japaneseInstalled={japaneseInstalled} back={() => open("home")} />}
                {current === "romaji" && <RomajiTable romaji={romaji} runAction={runAction} setConfirm={setConfirm} back={() => open("keys")} />}
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
          {confirm?.link && <button type="button" className="dialog-link" onClick={() => { const link = confirm.link; if (!link.keep) setConfirm(null); link.action(); }}>{confirm.link.label}</button>}
          <div className="dialog-actions">
            <Button variant={confirm?.danger ? "danger" : "primary"} onClick={async () => { const action = confirm.action; setConfirm(null); await action(); }}>{confirm?.confirmLabel}</Button>
            <Button onClick={() => setConfirm(null)}>{t.cancel}</Button>
          </div>
        </Dialog>
        <Dialog open={Boolean(keys)} title={t.keysTitle} onClose={() => setKeys(null)}>
          {keys && <KeyList japaneseKeyboard={japaneseKeyboard} settings={settings} japanese={japanese} />}
          <div className="dialog-actions"><Button variant="primary" onClick={() => setKeys(null)}>{t.close}</Button></div>
        </Dialog>
        <Toaster toasterId={toaster} />
      </div>
      </FluentProvider>
    </TextContext.Provider>
  );
}

// A page in the list; folded, its icon alone, named on hover.
function PageItem({ id, rail }) {
  const t = useText();
  return (
    <NavItem value={id} icon={<Symbol name={navIcons[id]} size={20} />} aria-label={rail ? t.pages[id] : undefined}
      title={rail ? t.pages[id] : undefined}>
      {rail ? null : t.pages[id]}
    </NavItem>
  );
}

// The English words' six engine policies come down to three behaviors.
const actionPolicies = { replace: "Correct spelling", suggest: "Suggest alternative", keep: "Protect original" };
function actionOf(policy) {
  if (policy === "Protect original") return "keep";
  if (policy === "Correct spelling" || policy === "Normalize") return "replace";
  return "suggest";
}

// A file dropped anywhere but a drop area is ignored (WebView2 would
// otherwise open it in place of Settings).
for (const name of ["dragover", "drop"]) window.addEventListener(name, (event) => event.preventDefault());

// ---------------------------------------------------------------------------
// 日本語

// 入力の見本: short clips of what each mode does (public/samples, recorded
// by tests/visual/samples/encode.py), one playing at a time.
const sampleSets = {
  japanese: [
    ["japanese-convert", ["Space", "Enter"]], ["japanese-convert-long", ["Space"]], ["japanese-live", []],
    ["japanese-phrases", ["Space", "↑"]], ["japanese-prediction", ["Tab"]], ["japanese-english", ["Shift"]],
    ["japanese-forms", ["F6", "F7", "F8", "F10"]], ["japanese-special", ["Space"]], ["japanese-undo", ["Ctrl", "Backspace"]],
    ["japanese-slips", ["Space"]],
  ],
  mixed: [["mixed-basic", ["Space"]], ["mixed-long", ["Space"]], ["mixed-live", []], ["mixed-vowel", ["Space"]], ["mixed-correction", ["Space"]]],
  auto: [
    ["auto-correct", ["Space"]], ["auto-undo", ["Backspace", "Esc"]], ["auto-completion", ["Tab"]], ["auto-next", ["Tab"]],
    ["auto-casual", []], ["auto-period", ["Enter"]],
  ],
};

function SamplesPage({ japaneseInstalled, back }) {
  const t = useText();
  const modes = japaneseInstalled ? ["japanese", "mixed", "auto"] : ["auto"];
  const [mode, setMode] = useState(modes[0]);
  const [shown, setShown] = useState(null);
  const theme = window.matchMedia?.("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  const clip = (id) => `./samples/${id}-${theme}.mp4`;
  return (
    <>
      <Button icon="chevron_left" variant="quiet" className="back" onClick={back}>{t.pages.home}</Button>
      <div className="sample-tabs" role="tablist" aria-label={t.pages.samples}>
        {modes.map((id) => (
          <button key={id} type="button" role="tab" aria-selected={mode === id} className="sample-tab" onClick={() => setMode(id)}>
            <ModeIcon id={id} size={18} />{t.modeNames[id]}
          </button>
        ))}
      </div>
      <div className="samples" role="tabpanel">
        {sampleSets[mode].map(([id, keys]) => (
          <button key={id} type="button" className="sample" onClick={() => setShown([id, keys])}>
            <span className="sample-stage">
              {/* The clip's last frame; the clip plays once opened. */}
              <img src={clip(id).replace(".mp4", ".jpg")} alt="" />
              <span className="sample-play" aria-hidden="true"><Symbol name="play_arrow" size={22} /></span>
            </span>
            <span className="sample-name"><span>{t.samples.names[id]}</span><Keycaps keys={keys} /></span>
          </button>
        ))}
      </div>
      <Dialog open={Boolean(shown)} title={shown ? t.samples.names[shown[0]] : ""} wide onClose={() => setShown(null)}>
        {shown && (
          <div className="sample-player">
            <video key={shown[0]} src={clip(shown[0])} muted loop playsInline autoPlay />
          </div>
        )}
        <div className="dialog-actions"><Button variant="primary" onClick={() => setShown(null)}>{t.close}</Button></div>
      </Dialog>
    </>
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
                <TextBox aria-label={r.keys} placeholder={r.keys} value={keys} onChange={(event) => setKeys(event.target.value)} />
                <TextBox aria-label={r.kana} placeholder={r.kana} value={kana} disabled={unused} onChange={(event) => setKana(event.target.value)} />
                <Checkbox checked={unused} label={r.unused} onChange={(event, data) => setUnused(Boolean(data.checked))} />
                <Button variant="primary" type="submit">{t.add}</Button>
              </form>
              <ul className="list" aria-label={r.title}>
                {romaji.map((row) => (
                  <li key={row.input}>
                    <span className="reading">{row.input}</span>
                    <span className="text">{row.output || r.unused}</span>
                    {row.standard && <span className="tag">{r.standard(row.standard)}</span>}
                    <IconButton icon="close" label={r.removeLabel(row.input)} onClick={() => runAction("romaji.delete", { input: row.input }, r.removed)} />
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
// 辞書と学習

function DictionaryPage({ settings, setSetting, runAction, setConfirm, japanese, learning, dictionary, japaneseWords, setModal, encryption }) {
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
                      <IconButton icon="close" label={t.words.removeLabel(word.surface)}
                        onClick={() => askDelete(t.japaneseWords.removeTitle, t.japaneseWords.removeMessage(word.surface),
                          () => runAction("japaneseWords.delete", { reading: word.reading, surface: word.surface }, t.japaneseWords.removed))} />
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
                  <TextBox aria-label={t.words.search} placeholder={t.words.search} value={query} onChange={(event) => setQuery(event.target.value)}
                    before={<Symbol name="search" size={18} />} />
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
                      <IconButton icon="close" label={t.words.removeLabel(entry.raw)}
                        onClick={() => askDelete(t.words.removeTitle, t.words.removeMessage(entry.raw),
                          () => runAction("dictionary.delete", { id: entry.id }, t.words.removed))} />
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
                  <TextBox aria-label={l.search} placeholder={l.search} value={learnedQuery} onChange={(event) => setLearnedQuery(event.target.value)}
                    before={<Symbol name="search" size={18} />} />
                </div>
              )}
              <ul className="list" aria-label={l.title}>
                {shownLearned.map((item) => (
                  <li key={`${item.kind}\u0000${item.key}\u0000${item.value}`}>
                    <span className="reading">{item.key}</span>
                    <span className="text">{item.value}</span>
                    <span className="tag">{l.how[item.how](item.count)}</span>
                    <IconButton icon="close" label={l.forgetLabel(item.value || item.key)} onClick={() => forgetLearned(item)} />
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
        <Row icon="lock" title={d.secret.title} description={d.secret.description}>
          <Toggle label={d.secret.title} checked={settings.secretMode} onChange={(value) => setSetting("secretMode", value)} />
        </Row>
        <Row icon="lock" title={d.private.title} description={d.private.description}>
          <Toggle label={d.private.title} checked={settings.noLearningInPrivate} onChange={(value) => setSetting("noLearningInPrivate", value)} />
        </Row>
      </Card>
      <Details id="dictionary" title={t.details}>
        <Card label={t.dictionary.encryption.title}>
          <EncryptionRow encryption={encryption} runAction={runAction} setConfirm={setConfirm} />
        </Card>
      </Details>
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
                        <IconButton icon="close" label={t.learningList.forgetLabel(item.text)} onClick={() => forgetRecent(item.id)} />
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
        <Row icon="delete" title={a.uninstall.title} description={a.uninstall.description}>
          <Button onClick={() => runAction("uninstall.open", {})}>{a.uninstall.button}</Button>
        </Row>
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
            <Button onClick={() => openNotice("fluentui-LICENSES.txt", "Fluent UI — MIT License", true)}>Fluent UI</Button>
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
      <Field label={t.words.whenITypeLabel}>
        <TextBox autoFocus value={value.raw} placeholder={t.words.whenITypePlaceholder} onChange={(event) => setValue({ ...value, raw: event.target.value })} />
      </Field>
      <Field label={t.words.actionLabel} hint={t.actions[value.action].help}>
        <Segmented label={t.words.actionLabel} value={value.action} options={Object.keys(actionPolicies).map((key) => [key, t.actions[key].label])}
          onChange={(action) => setValue({ ...value, action })} />
      </Field>
      {!keep && (
        <Field label={t.words.changeToLabel}>
          <TextBox value={value.candidate} placeholder={t.words.changeToPlaceholder} onChange={(event) => setValue({ ...value, candidate: event.target.value })} />
        </Field>
      )}
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
      <Field label={w.readingLabel} validationState={reading && !readingOk ? "error" : "none"}
        validationMessage={reading && !readingOk ? w.readingInvalid : undefined} hint={reading && !readingOk ? undefined : w.readingHelp}>
        <TextBox autoFocus value={value.reading} placeholder={w.readingPlaceholder} lang="ja"
          onChange={(event) => setValue({ ...value, reading: event.target.value })} />
      </Field>
      <Field label={w.surfaceLabel}>
        <TextBox value={value.surface} placeholder={w.surfacePlaceholder} lang="ja"
          onChange={(event) => setValue({ ...value, surface: event.target.value })} />
      </Field>
      <Field label={w.actionLabel} hint={w.actions[value.action].help}>
        <Segmented label={w.actionLabel} value={value.action} options={japaneseActions.map((action) => [action, w.actions[action].label])}
          onChange={(action) => setValue({ ...value, action })} />
      </Field>
      {value.action !== "suppress" && (
        <Field label={w.kindLabel}>
          <Segmented label={w.kindLabel} value={value.kind} options={japaneseKinds.map((kind) => [kind, w.kinds[kind]])}
            onChange={(kind) => setValue({ ...value, kind })} />
        </Field>
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
