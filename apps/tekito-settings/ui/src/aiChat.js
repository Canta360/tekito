// Home's AI: Settings' AI model (settings-ai) scores how likely each
// setting's description (aiDescriptions.js) reads after what the user asked
// for; words they share add to it. A clear first choice is changed (as far
// as aiSettingsControl allows, in home.jsx); otherwise the likeliest three
// are shown to choose from. Measured on 80 requests (.claude/docs is not
// shipped): the clear choices were right about 94% of the time.
import { hostRequest } from "./host.js";
import { aiDescriptions } from "./aiDescriptions.js";
import { asksWhat, pick, talk } from "./aiTalk.js";
import { aiSpecs, catalog, valueName, visible } from "./catalog.jsx";
import { mixPairs, revealHidden } from "./rows.jsx";

// Word overlap's weight beside the model's score, and how far the first
// choice must lead the second to be changed without asking.
const kOverlapWeight = 1.5;
const kSureMargin = 4;
// Below this, no setting fits the request at all (a right first choice
// scores about 40, a loose one 15).
const kNothingFits = 10;

const prompts = {
  ja: { ask: (q) => `ユーザーの頼み：${q}\nこれに当てはまる設定：`, value: (q, title) => `ユーザーの頼み：${q}\n「${title}」を次にする：` },
  en: { ask: (q) => `The user asks: ${q}\nThe setting that fits: `, value: (q, title) => `The user asks: ${q}\nSet "${title}" to: ` },
};

// Pairs of letters the request and a description share.
function grams(text) {
  const plain = [...text.toLowerCase()].filter((ch) => !/[\s、。，．（）「」？！?!・：:]/.test(ch)).join("");
  const out = new Set();
  for (let i = 0; i + 1 < plain.length; i += 1) out.add(plain.slice(i, i + 2));
  return out;
}
const overlap = (a, b) => {
  const left = grams(a);
  let n = 0;
  for (const g of grams(b)) if (left.has(g)) n += 1;
  return n;
};

async function score(prompt, base, texts) {
  const response = await hostRequest("ai.score", { prompt, base, texts: texts.join("\n") });
  return response.ok && Array.isArray(response.scores) && response.scores.length === texts.length ? response.scores : null;
}

// Wanting a thing off, or on, as people say it.
const wantsOff = /やめ|止め|消し|消して|消え|いらない|要らない|うざ|じゃま|邪魔|しないで|出さないで|出ないで|直さないで|変えないで|オフ|無効|使いたくない|覚えないで|残さず|残さない|嫌|不要|stop|don't|dont|turn off|disable|no more|annoying|without/i;
const wantsOn = /したい|ほしい|欲しい|出して|オンに|有効|使いたい|付けて|つけて|入れて|直して|turn on|enable|i want|please/i;
const wantsMore = /増|多く|多め|もっと|たくさん|more|increase|bigger/i;
const wantsFewer = /減|少な|少し|fewer|less|decrease|smaller/i;
// Which part of a row with several (widths, special) the request names.
const parts = {
  widths: [[/英字|アルファベット|ローマ字|letter|alphabet/i, "japaneseLetterWidth"], [/記号|！|？|!|\?|symbol/i, "japaneseSymbolWidth"],
    [/スペース|空白|space/i, "japaneseSpaceWidth"]],
  special: [[/日付|日にち|時刻|今日|きょう|date|today/i, "dateConversion"], [/記号|やじるし|矢印|symbol|arrow/i, "symbolConversion"],
    [/計算|足し算|calc|sum/i, "calculatorEnabled"]],
};

// The keys a change to entry `entry` sets: its own, with Mix's when the two
// modes are linked (the same values).
function keysFor(ctx, entry, key) {
  const keys = [key];
  const paired = (entry.linkedKeys || []).filter((other) => other !== key);
  const mixKey = mixPairs[key];
  for (const other of [...paired, ...(mixKey && ctx.on.mixed ? [mixKey] : [])]) {
    if (ctx.settings[other] === ctx.settings[key]) keys.push(other);
  }
  return keys;
}

// What to set `entry` to for `question`: { key, value, keys } or nothing
// (the row is shown to change by hand).
async function decide(ctx, entry, question, level, lang) {
  const specs = aiSpecs(ctx);
  let key = entry.linkedKeys?.[0] || entry.keys[0];
  if (parts[entry.id]) {
    const named = parts[entry.id].find(([pattern]) => pattern.test(question));
    if (!named) return null;
    key = named[1];
  }
  const spec = specs[key];
  if (!spec) return null;
  const current = ctx.settings[key];
  const values = spec.standard.map(([value]) => value);
  if (values.length === 2 && values.every((value) => typeof value === "boolean")) {
    const value = wantsOff.test(question) ? false : wantsOn.test(question) ? true : !current;
    return { key, value, keys: keysFor(ctx, entry, key) };
  }
  if (spec.range && (wantsMore.test(question) || wantsFewer.test(question))) {
    const [min, max] = spec.range;
    const now = typeof current === "number" && current > 0 ? current : 9;
    const value = wantsMore.test(question) ? (level >= 2 ? max : Math.max(...values.filter((v) => typeof v === "number"))) : Math.max(min, 5);
    if (value !== now) return { key, value, keys: [key] };
  }
  // A choice: the one whose name reads likeliest after the request.
  const options = spec.standard.filter(([value]) => value !== current);
  if (options.length === 0) return null;
  const p = prompts[lang];
  const scores = await score(p.value(question, entry.title), p.value("", entry.title), options.map(([, name]) => String(name)));
  if (!scores) return null;
  const best = options.map((option, i) => [option, scores[i] + kOverlapWeight * overlap(question, String(option[1]))])
    .sort((a, b) => b[1] - a[1])[0][0];
  return { key, value: best[0], keys: keysFor(ctx, entry, key) };
}

// The easter eggs, found by their names here as by search (with AI on,
// Home's box asks AI, not search): turned on (or off when asked), and
// shown on General from then on.
const eggs = [
  { id: "cat", key: "catSounds", names: /ねこ|猫|ネコ|にゃ|cats?|meow|kitty/i },
  { id: "rainbow", key: "rainbowAccent", names: /レインボー|にじ|虹|rainbow/i },
];

// One request: { reply, actions: [{ key, value }], show: [{ type, id }], title, label }
// or null when the model did not answer.
export async function think(ctx, question, level, lastApplied) {
  const t = ctx.t;
  const c = t.chat;
  const lang = t.lang === "ja" ? "ja" : "en";
  // "Put it back": the last changes undone.
  if (/元に戻|もとに戻|やっぱり|取り消|undo|revert|put it back/i.test(question) && lastApplied?.length) {
    return { kind: "undo", reply: c.undoneAll, actions: lastApplied.map((change) => ({ key: change.key, value: change.before })), show: [] };
  }
  const egg = eggs.find((one) => one.names.test(question));
  if (egg) {
    revealHidden(egg.id);
    const value = !wantsOff.test(question);
    return { kind: "egg", reply: c.eggs[egg.id](value), actions: [{ key: egg.key, value }], show: [{ type: "setting", id: egg.id }] };
  }
  // What it can do: said, not looked up.
  if (/何ができ|なにができ|できること|使い方|ヘルプ|what can you|help/i.test(question)) {
    return { kind: "help", reply: c.help, actions: [], show: [] };
  }
  // Small talk, and questions about TEKITO itself: answered, not looked up.
  const said = talk(question, lang);
  if (said) {
    const shown = said.show.filter((item) => item.type !== "setting" || catalog(ctx).some((entry) => entry.id === item.id && visible(entry, ctx)));
    return { kind: "talk", reply: said.reply, actions: [], show: shown };
  }
  const described = aiDescriptions[lang];
  const targets = [
    ...catalog(ctx).filter((entry) => !entry.hidden && visible(entry, ctx) && described[entry.id])
      .map((entry) => ({ type: "setting", id: entry.id, title: entry.title, entry, text: described[entry.id] })),
    ...["japaneseWords", "englishWords", "learned", "report"]
      .filter((id) => ctx.japaneseInstalled || (id !== "japaneseWords" && id !== "report"))
      .map((id) => ({ type: "page", id, title: "", text: described[id] })),
    ...(ctx.on.japanese ? [{ type: "keys", id: "japanese.kana", title: "", text: described["keys.kana"] }] : []),
  ];
  const p = prompts[lang];
  const scores = await score(p.ask(question), p.ask(""), targets.map((target) => target.text));
  if (!scores) return null;
  const ranked = targets.map((target, i) => ({ ...target, score: scores[i] + kOverlapWeight * overlap(question, target.text + target.title) }))
    .sort((a, b) => b.score - a.score);
  const [best, second] = ranked;
  if (best.score < kNothingFits) return { kind: "nothing", reply: pick(c.nothingFits), actions: [], show: [] };
  if (best.score - second.score < kSureMargin) {
    // As many as are close to the first (two to five), to choose from.
    const close = ranked.filter((target, i) => i < 5 && (i < 2 || best.score - target.score < kSureMargin * 2));
    return { kind: "choose", reply: c.choose, actions: [], show: close.map(({ type, id }) => ({ type, id })) };
  }
  const found = { kind: "found", reply: c.found, actions: [], show: [{ type: best.type, id: best.id }] };
  if (best.type !== "setting") return found;
  // Asked what it is (シークレットモードって何): told, not changed.
  if (asksWhat(question)) return best.entry.about ? { ...found, reply: c.about(best.entry.title, best.entry.about) } : found;
  const change = await decide(ctx, best.entry, question, level, lang);
  if (!change) return found;
  const spec = aiSpecs(ctx)[change.key];
  return {
    kind: "change", title: best.entry.title, label: valueName(spec, change.value, t),
    actions: change.keys.map((key) => ({ key, value: change.value })), show: [],
  };
}
