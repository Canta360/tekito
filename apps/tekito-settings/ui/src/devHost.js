// Development-only stand-in for the native host, so `vite` can render the
// page in a normal browser. Loaded only when import.meta.env.DEV is true and
// no WebView2 host is present; production builds drop it entirely.

const packNames = [
  "Standard English", "Common Misspellings", "Frequency", "Phrase / N-gram",
  "Dictionary Display", "Proper Nouns", "Slang", "Extended Slang", "Pronunciation",
  "Emoji", "Social Expressions", "Japanese Phonetic Suggestions", "QWERTY Typo Evaluation",
];
const japanesePackNames = [
  "Japanese Dictionary", "Romaji Table", "Japanese Word Pairs", "Loanwords",
  "Japanese Meanings (Wiktionary)", "Japanese Meanings (WordNet)",
];
// "?japanese=0" shows Settings as without Japanese installed.
const japaneseInstalled = new URLSearchParams(window.location.search).get("japanese") !== "0";
const detectedJapaneseKeyboard = new URLSearchParams(window.location.search).get("keyboard") !== "us";

function initialState() {
  return {
    postalCodes: { installed: false },
    settings: {
      restoreLastInputMode: true,
      correctionEnabled: true,
      commonMisspellingsEnabled: true,
      contextSuggestionsEnabled: true,
      completionEnabled: false,
      candidateWindowEnabled: true,
      learningEnabled: true,
      japanesePhoneticSuggestionsEnabled: false,
      socialExpressionRange: 1,
      socialPersonalization: 1,
      candidateWindowStyle: 0,
      candidateRows: 0,
      meaningsEnabled: true,
      japaneseSwitchOrder: 0,
      japaneseEnabled: true,
      modeIndicatorEnabled: true,
      dateConversion: true,
      numberConversion: true,
      symbolConversion: true,
      calculatorEnabled: true,
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
      uiLanguage: 0,
      excludedApps: ["Code.exe"],
      builtInExcludedApps: ["WindowsTerminal.exe", "OpenConsole.exe", "conhost.exe", "cmd.exe", "powershell.exe", "pwsh.exe"],
    },
    version: "0.4.1",
    mode: japaneseInstalled ? "japanese" : "auto",
    runtime: { tsf: "Loaded", dataPacks: "12 / 13", japaneseData: japaneseInstalled ? "6 / 6" : "0 / 6", japanese: japaneseInstalled,
      // "?keyboard=us": Windows reports a US keyboard.
      japaneseKeyboard: detectedJapaneseKeyboard },
    appearance: { accent: "#0078d4", systemLanguage: navigator.language.startsWith("ja") ? "ja" : "en" },
    learning: { enabled: true, count: 128 },
    japaneseWords: [
      { reading: "てきとう", surface: "TEKITO", kind: "proper-noun" },
      { reading: "かんた", surface: "カンタ", kind: "given-name", action: "suggest" },
      { reading: "きしゃ", surface: "汽車", kind: "noun", action: "suppress" },
    ],
    dictionary: [
      { id: 1, raw: "tekito", candidate: "tekito", type: "Word", policy: "Protect original" },
      { id: 2, raw: "brb", candidate: "be right back", type: "Word", policy: "Expand abbreviation" },
      { id: 3, raw: "omw", candidate: "on my way", type: "Word", policy: "Correct spelling" },
    ],
    packs: [...packNames.map((name, index) => ({
      displayName: name,
      group: "english",
      valid: index !== 5,
      version: "2026.09",
      packPath: `C:\\ProgramData\\TEKITO\\data\\pack-${index}`,
      reason: index === 5 ? "Checksum mismatch" : "",
      source: "Bundled",
      license: "CC BY-SA 4.0",
      noticePath: `C:\\ProgramData\\TEKITO\\data\\pack-${index}\\NOTICE.txt`,
    })), ...japanesePackNames.map((name, index) => ({
      displayName: name,
      group: "japanese",
      valid: japaneseInstalled,
      version: "2026.09",
      packPath: `C:\\ProgramData\\TEKITO\\data\\japanese-${index}`,
      reason: japaneseInstalled ? "" : "manifest missing or empty",
      source: "Bundled",
      license: "",
      noticePath: "",
    }))],
  };
}

export function installMockHost() {
  const state = initialState();
  const listeners = new Set();
  const reply = (requestId, ok = true, error = "") => {
    const data = JSON.stringify({ requestId, ok, error, state });
    window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
  };
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    addEventListener: (_type, listener) => listeners.add(listener),
    removeEventListener: (_type, listener) => listeners.delete(listener),
    postMessage: (raw) => {
      const message = JSON.parse(raw);
      const { type, requestId } = message;
      if (type === "japaneseWords.save") {
        const words = state.japaneseWords.filter((w) =>
          !(w.reading === message.originalReading && w.surface === message.originalSurface) &&
          !(w.reading === message.reading && w.surface === message.surface));
        const reading = [...message.reading].map((c) => {
          const code = c.charCodeAt(0);
          return code >= 0x30A1 && code <= 0x30F6 ? String.fromCharCode(code - 0x60) : c;
        }).join("");
        state.japaneseWords = [...words, { reading, surface: message.surface, kind: message.kind, action: message.action || "first" }];
      } else if (type === "japaneseWords.delete") {
        state.japaneseWords = state.japaneseWords.filter((w) => !(w.reading === message.reading && w.surface === message.surface));
      } else if (type === "settings.set") {
        state.settings[message.key] = message.value;
        if (message.key === "learningEnabled") state.learning.enabled = message.value;
        if (message.key === "japaneseEnabled" && japaneseInstalled) state.mode = message.value ? "japanese" : "auto";
        if (message.key === "keyboardType") {
          state.runtime.japaneseKeyboard = message.value === 0 ? detectedJapaneseKeyboard : message.value === 1;
        }
      } else if (type === "mode.set") {
        state.mode = message.value;
      } else if (type === "excludedApps.add") {
        const name = /\.[a-z0-9]+$/i.test(message.name) ? message.name : `${message.name}.exe`;
        if (!state.settings.excludedApps.some((app) => app.toLowerCase() === name.toLowerCase())) state.settings.excludedApps.push(name);
      } else if (type === "excludedApps.remove") {
        state.settings.excludedApps = state.settings.excludedApps.filter((app) => app !== message.name);
      } else if (type === "learning.clear") {
        state.learning.count = 0;
      } else if (type === "dictionary.create") {
        const id = Math.max(0, ...state.dictionary.map((entry) => entry.id)) + 1;
        state.dictionary.push({ id, raw: message.raw, candidate: message.candidate, type: "Word", policy: message.policy });
      } else if (type === "dictionary.update") {
        state.dictionary = state.dictionary.map((entry) => entry.id === message.id ? { ...entry, raw: message.raw, candidate: message.candidate, policy: message.policy } : entry);
      } else if (type === "dictionary.delete") {
        state.dictionary = state.dictionary.filter((entry) => entry.id !== message.id);
      }
      if (type === "postalCodes.import" || type === "postalCodes.browse") {
        state.postalCodes = { installed: true, version: "2026.09", count: 124182 };
      } else if (type === "postalCodes.remove") {
        state.postalCodes = { installed: false };
      }
      if (type === "japaneseWords.import" || type === "japaneseWords.browse") {
        state.japaneseWords = [...state.japaneseWords,
          { reading: "やまだ", surface: "山田", kind: "surname", action: "first" },
          { reading: "とうきょうえき", surface: "東京駅", kind: "place", action: "first" }];
        const data = JSON.stringify({ requestId, ok: true, error: "", state, text: "2 語を追加しました。" });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
        return;
      }
      if (type === "license.get") {
        const data = JSON.stringify({ requestId, ok: true, error: "", state, text: "# TEKITO License Agreement\n\nVersion 0.1 (preview)\n\n## In short\n\n- You may install TEKITO on the computers you use.\n- What you type is yours.\n\n## 1. What you may do\n\nWe give you a personal license to use TEKITO." });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
        return;
      }
      reply(requestId);
    },
  };
}
