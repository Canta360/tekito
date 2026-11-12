import licenseText from "../../../../installer/TEKITO_LICENSE.md?raw";

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
// "?ai=1": the light AI model is there and chosen, so Home asks AI.
const aiInstalled = new URLSearchParams(window.location.search).get("ai") === "1";

// A stand-in for AI in Home's conversation: a few requests as words to look
// for, and the reply, changes and pointers AI would give for them.
function fakeChat(messages, settings, level) {
  const answer = fakeReply(messages, settings, level);
  // Only suggesting, AI says so.
  if (level === 0 && answer.actions.length) answer.reply = answer.reply.replace(/にしました。/, "にする提案です。「適用」を押すと変更されます。").replace(/に戻しました。/, "に戻す提案です。「適用」を押すと変更されます。");
  return answer;
}

function fakeReply(messages, settings, level) {
  const said = messages.filter((m) => m.role === "user").map((m) => m.text);
  const last = said[said.length - 1] || "";
  const current = Object.fromEntries((settings || []).map((s) => [s.key, s.current]));
  const reply = (text, actions = [], show = []) => ({ reply: text, actions, show });
  if (/(auto|Auto|オート).*(だけ|は).*(直|修正|オン)/.test(last)) {
    return reply("Auto の入力ミスの修正をオンに戻しました。まぜ打ちはオフのままです。", [{ key: "correctionEnabled", value: true }]);
  }
  if (/英語|英単語|english/i.test(last) && /直|修正|correct|fix/i.test(last)) {
    return reply("英単語の入力ミスの修正をオフにしました。特定の単語だけ直されたくない場合は、英語の単語に登録すると、その語だけ修正されなくなります。",
      [{ key: "mixCorrection", value: false }, { key: "correctionEnabled", value: false }], [{ type: "page", id: "englishWords" }]);
  }
  if (/候補/.test(last) && /増|多|ふや/.test(last)) {
    const big = /めっちゃ|もっと|最大|すごく|たくさん/.test(last);
    const rows = big ? 20 : 9;
    if (rows > 9 && level < 2) {
      return reply(`1ページの候補数を${rows}件にする提案です。選択肢にない数なので、「適用」を押すと変更されます。数字キーで選べるのは9件目までです。`,
        [{ key: "candidateRows", value: rows }]);
    }
    return reply(rows > 9 ? `1ページの候補数を${rows}件にしました。数字キーで選べるのは9件目までで、10件目からは矢印キーで選びます。`
      : `1ページの候補数を${rows}件にしました。`, [{ key: "candidateRows", value: rows }]);
  }
  if (/候補/.test(last) && /減|少|へら/.test(last)) {
    return reply("1ページの候補数を5件にしました。", [{ key: "candidateRows", value: 5 }]);
  }
  if (/全角|半角/.test(last)) {
    const full = /全角に|全角で/.test(last);
    if (/英字|アルファベット/.test(last)) {
      return reply(`英字を${full ? "全角" : "半角"}にしました。`, [{ key: "japaneseLetterWidth", value: full ? 0 : 1 }]);
    }
    return reply("全角と半角は、英字・記号・スペースで別々に選べます。どれを変えますか？", [], [{ type: "setting", id: "widths" }]);
  }
  if (/カタカナ/.test(last)) return reply("変換中に F7 を押すとカタカナになります。", [], [{ type: "keys", id: "japanese.kana" }]);
  if (/漢字/.test(last) && /減|子ども|子供|少|簡単/.test(last)) {
    return reply("変換に使う漢字を小学3年生までにしました。", [{ key: "japaneseKanjiRange", value: 3 }, { key: "japaneseKanjiGrade", value: 3 }]);
  }
  if (/変な|おかしい|間違/.test(last)) {
    return reply("よく間違える語は、単語を登録するか、学習した内容から消すと直ります。直らない場合は、誤変換として報告できます。", [],
      [{ type: "page", id: "japaneseWords" }, { type: "page", id: "learned" }, { type: "page", id: "report" }]);
  }
  if (/ライブ/.test(last)) {
    const off = /やめ|オフ|切/.test(last);
    return reply(`ライブ変換を${off ? "オフ" : "オン"}にしました。`, [{ key: "japaneseLiveConversion", value: !off }, { key: "mixLiveConversion", value: !off }]);
  }
  if (/元に戻|やっぱり/.test(last) && current.candidateRows !== undefined && said.length > 1) {
    return reply("1ページの候補数を自動に戻しました。", [{ key: "candidateRows", value: 0 }]);
  }
  return reply("うまく読み取れませんでした。困っていることや、変えたいことを言い換えてみてください。");
}

function initialState() {
  return {
    postalCodes: { installed: false },
    encryption: { wanted: false, encrypted: false },
    aiModels: { light: { installed: aiInstalled, bytes: 358021668 }, strong: { installed: false, bytes: 1587553900 },
      settings: { installed: aiInstalled && new URLSearchParams(window.location.search).get("settingsai") !== "0", bytes: 357791102 },
      runtime: { installed: aiInstalled, bytes: 6000000, windowsMl: false, windowsMlBytes: 20000000, windowsMlDownloading: false },
      downloading: 0, error: "",
      gpus: [{ id: 268566271, name: "AMD Radeon RX 7600" }, { id: -2142881189, name: "Intel(R) Arc(TM) Graphics" }],
      npu: true, npuPreparing: false, npuError: "" },
    settings: {
      startMode: 0,
      switchOrder: 1234,
      japaneseKanjiNames: false,
      mixCorrection: true,
      spacelessEnglish: true,
      spacelessEnglishAlways: false,
      mixSettledMarker: true,
      mixEnglishUnderline: false,
      rainbowAccent: false,
      catSounds: false,
      desktopCompanion: false,
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
      mixChunkCandidates: true,
      mixLiveRefine: true,
      mixEnglishSpacing: false,
      meaningsEnabled: true,
      meaningsInUiLanguage: true,
      japaneseSwitchOrder: 0,
      switchModes: 7,
      mixPredictionEnabled: true,
      mixFirstPageSymbol: true,
      japaneseEnabled: true,
      modeIndicatorEnabled: true,
      modeUnderlineColors: true,
      modeBadgeOnFocus: true,
      modePerApp: false,
      dateConversion: true,
      numberConversion: true,
      symbolConversion: true,
      calculatorEnabled: true,
      toggleKey: 1,
      keyboardType: 0,
      periodOnEnter: false,
      mixedTypingEnabled: false,
      japaneseEnglishSpacing: false,
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
      japaneseSymbolWidth: 0,
      japaneseLetterWidth: 1,
      advancedSettings: false,
      keepRecentConversions: true,
      japanesePredictionEnabled: true,
      japaneseLiveConversion: false,
      japaneseSentenceCheck: true,
      japaneseSentenceModel: aiInstalled ? 1 : 0,
      japaneseAiDevice: 0,
      japaneseAiGpu: 0,
      japaneseSlipAutoApply: true,
      japaneseFirstPageSymbol: true,
      japaneseTypedForms: true,
      japaneseChunkCandidates: true,
      japaneseKanjiVariants: true,
      japaneseLiveRefine: true,
      japaneseKanaInput: false,
      japaneseKanaInputKey: true,
      mixSingleVowelKana: true,
      mixLiveConversion: false,
      noLearningInPrivate: true,
      secretMode: false,
      japaneseIgnoreCapsLock: true,
      japaneseNumpadDirect: true,
      uiLanguage: 0,
      excludedApps: ["Code.exe"],
      builtInExcludedApps: ["WindowsTerminal.exe", "OpenConsole.exe", "conhost.exe", "cmd.exe", "powershell.exe", "pwsh.exe"],
    },
    version: "0.7.3",
    mode: japaneseInstalled ? "japanese" : "auto",
    runtime: { tsf: "Loaded", dataPacks: "12 / 13", japaneseData: japaneseInstalled ? "6 / 6" : "0 / 6", japanese: japaneseInstalled,
      // "?keyboard=us": Windows reports a US keyboard.
      japaneseKeyboard: detectedJapaneseKeyboard, japaneseDataBytes: japaneseInstalled ? 0 : 92000000 },
    appearance: { accent: "#0078d4", systemLanguage: navigator.language.startsWith("ja") ? "ja" : "en" },
    learning: { enabled: true, count: 128 },
    learned: [
      { kind: "ja", key: "きしゃ", value: "記者", how: "chosen", count: 3 },
      { kind: "ja", key: "こうえん", value: "講演", how: "chosen", count: 1 },
      { kind: "en", key: "teh", value: "the", how: "chosen", count: 5 },
      { kind: "en", key: "github", value: "GitHub", how: "undone", count: 1 },
      { kind: "word", key: "tekito", value: "", how: "kept", count: 2 },
    ],
    romaji: [
      { input: "la", output: "ら", pending: "", standard: "ぁ" },
      { input: "nn", output: "", pending: "", standard: "ん" },
    ],
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
      } else if (type === "recent.list" || type === "recent.forget" || type === "recent.report") {
        state.recent = state.recent || [
          { id: 3, reading: "きかいがうごく", text: "機会が動く", mode: "Japanese", time: 0 },
          { id: 2, reading: "きょうは meeting", text: "今日はmeeting", mode: "Mix", time: 0 },
          { id: 1, reading: "ありがとう", text: "ありがとう", mode: "Japanese", time: 0 },
        ];
        if (type === "recent.forget") state.recent = message.id ? state.recent.filter((item) => item.id !== message.id) : [];
        const data = JSON.stringify({ requestId, ok: true, error: "", state, items: state.recent });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
        return;
      } else if (type === "learning.list" || type === "learning.forget") {
        if (type === "learning.forget") {
          state.learned = state.learned.filter((item) => !(item.kind === message.kind && item.key === message.key && item.value === message.value));
        }
        const data = JSON.stringify({ requestId, ok: true, error: "", state, items: state.learned });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
        return;
      } else if (type === "romaji.save") {
        const standard = { la: "ぁ", nn: "ん", ka: "か" }[message.input] || "";
        state.romaji = [...state.romaji.filter((row) => row.input !== message.input),
          { input: message.input, output: message.output, pending: message.pending, standard }];
      } else if (type === "romaji.delete") {
        state.romaji = state.romaji.filter((row) => row.input !== message.input);
      } else if (type === "romaji.reset") {
        state.romaji = [];
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
      if (type === "aiModel.download") {
        state.aiModels = { ...state.aiModels, downloading: message.model };
        window.setTimeout(() => {
          const key = message.model === 1 ? "light" : "strong";
          state.aiModels = { ...state.aiModels, [key]: { ...state.aiModels[key], installed: true },
            runtime: { ...state.aiModels.runtime, installed: true }, downloading: 0 };
          state.settings.japaneseSentenceModel = message.model;
          const data = JSON.stringify({ type: "state.changed", state });
          listeners.forEach((listener) => listener({ data }));
        }, 2500);
      } else if (type === "encryption.enable" || type === "encryption.renew") {
        state.encryption = { wanted: true, encrypted: state.encryption.encrypted };
        const data = JSON.stringify({ requestId, ok: true, error: "", state, code: "7K2M-Q9XA-3HPD-W8NB-5RTC-V6JE" });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
        return;
      } else if (type === "encryption.disable") {
        state.encryption = { wanted: false, encrypted: state.encryption.encrypted };
      } else if (type === "aiRuntime.windowsMl") {
        state.aiModels = { ...state.aiModels, runtime: { ...state.aiModels.runtime, windowsMlDownloading: true } };
        window.setTimeout(() => {
          state.aiModels = { ...state.aiModels, runtime: { ...state.aiModels.runtime, windowsMl: true, windowsMlDownloading: false } };
          state.settings.japaneseAiDevice = message.device;
          const data = JSON.stringify({ type: "state.changed", state });
          listeners.forEach((listener) => listener({ data }));
        }, 2500);
      } else if (type === "aiNpu.prepare") {
        state.aiModels = { ...state.aiModels, npuPreparing: true, npuError: "" };
        window.setTimeout(() => {
          state.aiModels = { ...state.aiModels, npuPreparing: false };
          const data = JSON.stringify({ type: "state.changed", state });
          listeners.forEach((listener) => listener({ data }));
        }, 2500);
      } else if (type === "aiModel.remove") {
        const key = message.model === 1 ? "light" : "strong";
        state.aiModels = { ...state.aiModels, [key]: { ...state.aiModels[key], installed: false } };
        if (!state.aiModels.light.installed && !state.aiModels.strong.installed) {
          state.aiModels = { ...state.aiModels, runtime: { ...state.aiModels.runtime, installed: false } };
        }
        if (state.settings.japaneseSentenceModel === message.model) state.settings.japaneseSentenceModel = 0;
      }
      if (type === "postalCodes.download") {
        window.setTimeout(() => {
          state.postalCodes = { installed: true, version: "2026.09", count: 124182 };
          reply(requestId);
        }, 1800);
        return;
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
      if (type === "japaneseData.install") {
        state.runtime = { ...state.runtime, japaneseInstalling: true };
        window.setTimeout(() => {
          state.runtime = { ...state.runtime, japanese: true, japaneseInstalling: false };
          state.settings.japaneseEnabled = true;
          state.mode = "japanese";
          const data = JSON.stringify({ type: "state.changed", state });
          listeners.forEach((listener) => listener({ data }));
        }, 2500);
      }
      if (type === "ai.score") {
        // A stand-in for Settings' AI model: letters each text shares with
        // the request, beyond what it shares with no request.
        const pairs = (text) => new Set([...text].map((ch, i, all) => ch + (all[i + 1] || "")).slice(0, -1));
        const shared = (a, b) => [...pairs(b)].filter((pair) => pairs(a).has(pair)).length;
        const scores = String(message.texts).split("\n").map((text) => 2 * (shared(message.prompt, text) - shared(message.base, text)));
        const data = JSON.stringify({ requestId, ok: true, error: "", state, scores });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 500);
        return;
      }
      if (type === "settingsAi.download") {
        state.aiModels = { ...state.aiModels, settings: { ...state.aiModels.settings, downloading: true } };
        window.setTimeout(() => {
          state.aiModels = { ...state.aiModels, settings: { ...state.aiModels.settings, installed: true, downloading: false },
            runtime: { ...state.aiModels.runtime, installed: true } };
          const data = JSON.stringify({ type: "state.changed", state });
          listeners.forEach((listener) => listener({ data }));
        }, 2500);
      }
      if (type === "settings.chat") {
        const data = JSON.stringify({ requestId, ok: true, error: "", state, ...fakeChat(message.messages || [], message.settings, message.level) });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 900);
        return;
      }
      if (type === "license.get") {
        const data = JSON.stringify({ requestId, ok: true, error: "", state, text: licenseText });
        window.setTimeout(() => listeners.forEach((listener) => listener({ data })), 60);
        return;
      }
      reply(requestId);
    },
  };
}
