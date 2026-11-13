// Every setting once: its page, group, the modes it applies to, the keys it
// stores, and its row. The pages, Home's search and AI answers, and All
// settings are drawn from this list; an id is what an AI answer names.
import React from "react";
import { Card, Check, Details, Heading, Keycaps, LinkRow, Row, Segmented, Select, Slider, Toggle } from "./ui.jsx";
import {
  AiModelRow, candidateRowsRange, ExcludedApps, kanjiStop, kanjiStops, LinkedStyle, LinkedToggle, mixPairs, ModeBadges, NumbersRow,
  PostalCodes, RowsRow, SpecialRow,
  EncryptionRow, SwitchOrder, switchKeyCaps, wasFound, WidthsRow,
} from "./rows.jsx";

// The pages in the list's order; All settings and About sit at its foot.
export const navPages = ["home", "general", "conversion", "slips", "notation", "candidates", "keys", "dictionary"];
export const footPages = ["all", "about"];
export const navIcons = {
  home: "home", general: "settings", conversion: "translate", slips: "spellcheck", notation: "edit_note",
  candidates: "list_alt", keys: "keyboard", dictionary: "menu_book", all: "checklist", about: "info",
};
// The pages drawn from the catalog, and their groups in order.
export const pageGroups = {
  general: ["switching", "display", "apps", "general"],
  conversion: ["convert", "ai", "predict"],
  slips: ["japanese", "english"],
  notation: ["japanese", "english"],
  candidates: ["look", "list", "extra"],
  keys: ["keyboard", "typing"],
};

export function catalog(ctx) {
  const { t, settings: s, setSetting: set, on } = ctx;
  const g = t.general;
  const j = t.japanese;
  const m = t.mixed;
  const a = t.auto;
  const badges = (modes) => (ctx.multi ? <ModeBadges ids={modes.filter((mode) => on[mode])} /> : null);
  const head = (title, modes) => (modes ? <>{title}{badges(modes)}</> : title);
  // A switch: [id, page, group, icon, text, key, options].
  const toggle = (id, page, group, icon, text, key, opts = {}) => ({
    id, page, group, keys: [key, ...(opts.keys || [])], title: text.title, about: text.description, modes: opts.modes, detail: opts.detail, show: opts.show,
    render: () => (
      <Row icon={icon} title={head(text.title, opts.modes)} description={text.description} sub={opts.sub?.()}>
        <Toggle label={text.title} checked={s[key]} disabled={opts.disabled} onChange={(value) => set(key, value)} />
      </Row>
    ),
  });
  const linked = (id, page, group, icon, text, keys, opts = {}) => ({
    id, page, group, keys: [...Object.values(keys), ...(opts.keys || [])], title: text.title, about: text.description, modes: Object.keys(keys), detail: opts.detail,
    linkedKeys: Object.entries(keys).filter(([mode]) => on[mode]).map(([, key]) => key),
    render: () => (
      <LinkedToggle id={id} icon={icon} title={text.title} description={text.description} keys={keys} settings={s} setSetting={set}
        on={on} multi={ctx.multi} sub={opts.sub?.()} />
    ),
  });
  const choice = (id, page, group, icon, text, key, options, opts = {}) => ({
    id, page, group, keys: [key], title: text.title, about: text.description, modes: opts.modes, detail: opts.detail, show: opts.show,
    render: () => (
      <Row icon={icon} title={head(text.title, opts.modes)} description={text.description}>
        <Segmented label={text.title} value={opts.value ?? s[key]} options={options} onChange={(value) => set(key, value)} />
      </Row>
    ),
  });
  const ja = ["japanese", "mixed"];
  const en = ["mixed", "auto"];

  const startModes = [[0, g.startMode.last]].concat(
    (ctx.japaneseInstalled ? [[1, "japanese"], [2, "mixed"]] : []).concat([[3, "auto"], [4, "direct"]])
      .filter(([, mode]) => on[mode]).map(([value, mode]) => [value, t.modeNames[mode]]));
  const toggleKeys = [1, 2, 3].map((value) => {
    const caps = switchKeyCaps(value, ctx.japaneseKeyboard, t);
    return [value, <Keycaps keys={caps} />, caps.join(" + ")];
  }).concat([[0, g.switchKey.none]]);
  const gpus = ctx.aiModels.gpus || [];
  const setKanji = (put) => async (value) => {
    if (value <= 6) {
      await put("japaneseKanjiRange", 3);
      await put("japaneseKanjiGrade", value);
    } else {
      await put("japaneseKanjiRange", value === 7 ? 2 : 0);
    }
  };
  const setNumbers = (put) => async (value) => {
    await put("japaneseNumberStyle", value);
    if (value <= 3) await put("japaneseDigitWidth", value - 1);
  };
  // Japanese mode's ways of writing, with Mix's beside them.
  const styled = (entry) => ({
    ...entry,
    keys: [...entry.keys, ...entry.keys.map((key) => mixPairs[key]).filter(Boolean)],
    render: () => <LinkedStyle id={entry.id} keys={entry.keys} settings={s} setSetting={set} on={on} render={entry.draw} />,
  });
  const styledToggle = (id, page, group, icon, text, key, opts = {}) =>
    linked(id, page, group, icon, text, { japanese: key, mixed: mixPairs[key] }, opts);
  const spaceless = !s.spacelessEnglish ? 0 : s.spacelessEnglishAlways ? 2 : 1;
  const setSpaceless = async (value) => {
    await set("spacelessEnglish", value > 0);
    if (value > 0) await set("spacelessEnglishAlways", value === 2);
  };

  // Japanese and Mix need the Japanese data: downloaded once the user agrees.
  const addJapanese = () => ctx.setConfirm({
    title: t.modes.addTitle,
    message: t.modes.addBody(ctx.japaneseDataBytes),
    confirmLabel: t.modes.download,
    action: () => ctx.runAction("japaneseData.install", {}, t.modes.added),
  });

  return [
    // 一般
    {
      id: "switchKey", page: "general", group: "switching", keys: ["toggleKey"], title: g.switchKey.title,
      render: () => (
        <Row icon="keyboard" title={g.switchKey.title}>
          <Segmented label={g.switchKey.title} value={s.toggleKey} options={toggleKeys} onChange={(value) => set("toggleKey", value)} />
        </Row>
      ),
    },
    {
      id: "switchOrder", page: "general", group: "switching", keys: ["switchOrder", "switchModes", "japaneseEnabled", "mixedTypingEnabled"],
      title: g.order.title, find: [t.modes.find(t.modeNames.japanese), t.modes.find(t.modeNames.mixed), t.modes.add],
      render: () => (
        <SwitchOrder settings={s} setSetting={set} japaneseInstalled={ctx.japaneseInstalled} japaneseInstalling={ctx.japaneseInstalling}
          addJapanese={ctx.japaneseAddable ? addJapanese : null} japaneseError={ctx.japaneseDataError} />
      ),
    },
    {
      id: "startMode", page: "general", group: "switching", keys: ["startMode"], title: g.startMode.title, detail: true,
      render: () => (
        <Row icon="power_settings_new" title={g.startMode.title}>
          <Select label={g.startMode.title} value={startModes.some(([value]) => value === s.startMode) ? s.startMode : 0}
            options={startModes} onChange={(value) => set("startMode", value)} />
        </Row>
      ),
    },
    toggle("modePerApp", "general", "switching", "apps", g.modePerApp, "modePerApp", { detail: true }),
    toggle("indicator", "general", "display", "tooltip", g.indicator, "modeIndicatorEnabled", {
      keys: ["modeBadgeOnFocus"],
      sub: () => s.modeIndicatorEnabled && (
        <Check label={g.indicator.onFocus} checked={s.modeBadgeOnFocus} onChange={(value) => set("modeBadgeOnFocus", value)} />
      ),
    }),
    toggle("underline", "general", "display", "format_underlined", g.underline, "modeUnderlineColors", { detail: true }),
    toggle("mixMarker", "general", "display", "ink_highlighter", m.marker, "mixSettledMarker", { detail: true, modes: ["mixed"] }),
    toggle("mixEnglishUnderline", "general", "display", "format_underlined", m.englishUnderline, "mixEnglishUnderline",
      { detail: true, modes: ["mixed"] }),
    {
      id: "excludedApps", page: "general", group: "apps", keys: ["excludedApps"], title: g.excluded.title, detail: true,
      render: () => <ExcludedApps settings={s} runAction={ctx.runAction} />,
    },
    toggle("companion", "general", "general", "mood", g.companion, "desktopCompanion", { detail: true }),
    choice("language", "general", "general", "language", g.language, "uiLanguage", [[0, g.language.system], [1, "English"], [2, "日本語"]],
      { detail: true }),
    { ...toggle("rainbow", "general", "general", "palette", g.rainbow, "rainbowAccent", { detail: true }), hidden: "rainbow",
      find: ["rainbow", "レインボー", "にじ", "虹"] },
    { ...toggle("cat", "general", "general", "pets", g.cat, "catSounds", { detail: true }), hidden: "cat",
      find: ["cat", "ねこ", "猫", "ネコ"] },

    // 変換
    linked("live", "conversion", "convert", "bolt", j.live, { japanese: "japaneseLiveConversion", mixed: "mixLiveConversion" }, {
      keys: ["japaneseLiveRefine", "mixLiveRefine"],
      sub: () => (s.japaneseLiveConversion || s.mixLiveConversion) && (
        <Check label={j.live.refine} checked={s.japaneseLiveRefine}
          onChange={async (value) => { await set("japaneseLiveRefine", value); await set("mixLiveRefine", value); }} />
      ),
    }),
    styledToggle("sentenceCheck", "conversion", "convert", "notes", j.sentenceCheck, "japaneseSentenceCheck", { detail: true }),
    choice("singleVowel", "conversion", "convert", "text_format", m.singleVowel, "mixSingleVowelKana",
      [[true, m.singleVowel.kana], [false, m.singleVowel.letter]], { modes: ["mixed"], detail: true }),
    {
      id: "aiModel", page: "conversion", group: "ai", keys: ["japaneseSentenceModel", "japaneseAiDevice"], title: t.ai.title, modes: ja,
      render: () => (
        <AiModelRow settings={s} setSetting={set} runAction={ctx.runAction} setConfirm={ctx.setConfirm} aiModels={ctx.aiModels} badges={badges(ja)} />
      ),
    },
    choice("aiControl", "conversion", "ai", "tune", t.aiControl, "aiSettingsControl",
      t.aiControl.options.map((name, value) => [value, name]), { show: Boolean(ctx.aiReady) }),
    {
      id: "aiGpu", page: "conversion", group: "ai", keys: ["japaneseAiGpu"], title: t.ai.device.gpu, modes: ja, detail: true,
      show: (s.japaneseSentenceModel || 0) > 0 && ctx.aiModels.runtime?.windowsMl && s.japaneseAiDevice === 2 && gpus.length > 1,
      render: () => (
        <Row icon="memory" title={t.ai.device.gpu}>
          <Select label={t.ai.device.gpu} value={gpus.some((gpu) => gpu.id === s.japaneseAiGpu) ? s.japaneseAiGpu : 0}
            options={[[0, t.ai.device.auto], ...gpus.map((gpu) => [gpu.id, gpu.name])]} onChange={(value) => set("japaneseAiGpu", value)} />
        </Row>
      ),
    },
    linked("prediction", "conversion", "predict", "lightbulb", j.prediction, { japanese: "japanesePredictionEnabled", mixed: "mixPredictionEnabled" }),
    toggle("completion", "conversion", "predict", "short_text", a.completion, "completionEnabled", { modes: ["auto"] }),
    toggle("nextWord", "conversion", "predict", "text_select_end", a.next, "nextWordPrediction", { modes: ["auto"] }),
    {
      id: "casual", page: "conversion", group: "predict", keys: ["socialExpressionRange"], title: a.casual.title, modes: ["auto"],
      render: () => (
        <Row icon="forum" title={head(a.casual.title, ["auto"])} description={a.casual.description}>
          <Slider label={a.casual.title} value={s.socialExpressionRange} stops={a.casual.stops.map((name, value) => [value, name])}
            onChange={(value) => set("socialExpressionRange", value)} />
        </Row>
      ),
    },
    toggle("phonetic", "conversion", "predict", "record_voice_over", a.phonetic, "japanesePhoneticSuggestionsEnabled",
      { modes: ["auto"], detail: true }),

    // 入力ミスの修正
    styledToggle("slipAutoApply", "slips", "japanese", "spellcheck", j.slipAutoApply, "japaneseSlipAutoApply"),
    linked("englishCorrection", "slips", "english", "spellcheck", a.correction, { mixed: "mixCorrection", auto: "correctionEnabled" }, {
      keys: ["capitalizedCorrection"],
      sub: () => s.correctionEnabled && (
        <Check label={a.capitalized.title} example={t.slips.capitalizedExample} checked={s.capitalizedCorrection}
          onChange={(value) => set("capitalizedCorrection", value)} />
      ),
    }),
    choice("spelling", "slips", "english", "flag", a.spelling, "englishSpelling", a.spelling.options.map((name, value) => [value, name]), { modes: en }),
    {
      id: "spaceless", page: "slips", group: "english", keys: ["spacelessEnglish", "spacelessEnglishAlways"], title: a.spaceless.title, modes: en,
      render: () => (
        <Row icon="space_bar" title={head(a.spaceless.title, en)} description={a.spaceless.description}>
          <Segmented label={a.spaceless.title} value={spaceless} options={t.slips.spaceless.map((name, value) => [value, name])}
            onChange={setSpaceless} />
        </Row>
      ),
    },
    toggle("doubled", "slips", "english", "repeat", a.doubled, "doubledWordCheck", { modes: ["auto"], detail: true }),
    toggle("recheck", "slips", "english", "undo", a.recheck, "recheckPreviousWord", { modes: ["auto"], detail: true }),

    // 表記
    styled({
      id: "punctuation", page: "notation", group: "japanese", modes: ja, title: j.punctuation.title, keys: ["japanesePunctuation"],
      draw: (shown, put) => (
        <Row icon="edit_note" title={head(j.punctuation.title, ja)}>
          <Segmented label={j.punctuation.title} value={shown.japanesePunctuation} options={[[0, "、。"], [1, "，．"], [2, "，。"], [3, "、．"]]}
            onChange={(value) => put("japanesePunctuation", value)} />
        </Row>
      ),
    }),
    styled({
      id: "numbers", page: "notation", group: "japanese", modes: ja, title: j.numbers.title,
      keys: ["japaneseNumberStyle", "japaneseDigitWidth", "japaneseRoughNumbersKanji", "numberConversion"],
      draw: (shown, put) => (
        <NumbersRow settings={shown} setSetting={put} onChange={setNumbers(put)} badges={badges(ja)}
          options={j.numbers.options.map(([name, example], index) => [index + 1, name, example])} />
      ),
    }),
    styled({
      id: "widths", page: "notation", group: "japanese", modes: ja, title: t.widths.title,
      keys: ["japaneseLetterWidth", "japaneseSymbolWidth", "japaneseSpaceWidth"],
      draw: (shown, put) => <WidthsRow settings={shown} setSetting={put} badges={badges(ja)} />,
    }),
    styled({
      id: "kanji", page: "notation", group: "japanese", modes: ja, title: j.kanji.title,
      keys: ["japaneseKanjiRange", "japaneseKanjiGrade", "japaneseKanjiNames"],
      draw: (shown, put) => {
        const stop = kanjiStop(shown);
        return (
          <Row icon={<span className="row-glyph" aria-hidden="true">漢</span>} title={head(j.kanji.title, ja)} description={j.kanji.description}
            sub={stop < 8 && (
              <Check label={j.kanji.names} example={j.kanji.namesExample} checked={shown.japaneseKanjiNames}
                onChange={(value) => put("japaneseKanjiNames", value)} />
            )}>
            <span style={{ display: "grid", gap: 4 }}>
              <Slider label={j.kanji.title} value={stop} stops={kanjiStops.map((value, index) => [value, j.kanji.stops[index]])}
                onChange={setKanji(put)} />
              <small className="muted">{j.kanji.examples[stop - 1]}</small>
            </span>
          </Row>
        );
      },
    }),
    styledToggle("spacing", "notation", "japanese", "format_letter_spacing", j.spacing, "japaneseEnglishSpacing", { detail: true }),
    styledToggle("kanjiVariants", "notation", "japanese", "ink_highlighter", j.kanjiVariants, "japaneseKanjiVariants", { detail: true }),
    toggle("period", "notation", "english", "keyboard_return", a.period, "periodOnEnter", { modes: en }),
    toggle("doubleSpace", "notation", "english", "space_bar", a.doubleSpace, "doubleSpacePeriod", { modes: ["auto"], detail: true }),
    toggle("smart", "notation", "english", "format_quote", a.smart, "smartPunctuation", { modes: ["auto"], detail: true }),

    // 候補一覧
    {
      id: "looks", page: "candidates", group: "look", keys: ["candidateWindowStyle"], title: g.candidates, bare: true,
      render: () => (
        <div className="looks" role="radiogroup" aria-label={g.candidates}>
          {["glass", "simple"].map((look, value) => (
            <button key={look} type="button" role="radio" aria-checked={s.candidateWindowStyle === value} className="look"
              onClick={() => set("candidateWindowStyle", value)}>
              <picture>
                <source media="(prefers-color-scheme: dark)" srcSet={`./assets/candidates/${look}-dark.png`} />
                <img src={`./assets/candidates/${look}-light.png`} alt="" />
              </picture>
              {g.styles[look]}
            </button>
          ))}
        </div>
      ),
    },
    {
      id: "rows", page: "candidates", group: "list", keys: ["candidateRows"], title: g.rows.title,
      render: () => <RowsRow settings={s} setSetting={set} title={g.rows.title} description={g.rows.description} />,
    },
    toggle("meanings", "candidates", "list", "menu_book", g.meanings, "meaningsEnabled", {
      keys: ["meaningsInUiLanguage"],
      sub: () => s.meaningsEnabled && ctx.japaneseInstalled && (
        <Check label={g.meaningsLanguage.title} example={g.meaningsLanguage.description} checked={s.meaningsInUiLanguage}
          onChange={(value) => set("meaningsInUiLanguage", value)} />
      ),
    }),
    toggle("candidateWindow", "candidates", "list", "list_alt", g.candidateList, "candidateWindowEnabled", { detail: true }),
    {
      id: "special", page: "candidates", group: "extra", keys: ["dateConversion", "symbolConversion", "calculatorEnabled"], title: t.special.title,
      find: [g.date.title, g.symbols.title, g.calculator.title],
      render: () => <SpecialRow settings={s} setSetting={set} />,
    },
    {
      id: "postal", page: "candidates", group: "extra", keys: [], title: j.postal.title, modes: ja, show: ctx.japaneseInstalled,
      render: () => <PostalCodes postalCodes={ctx.postalCodes} runAction={ctx.runAction} setConfirm={ctx.setConfirm} />,
    },
    linked("firstPageSymbol", "candidates", "extra", "mood", j.firstPageSymbol, { japanese: "japaneseFirstPageSymbol", mixed: "mixFirstPageSymbol" },
      { detail: true }),
    styledToggle("typedForms", "candidates", "extra", "text_fields", j.typedForms, "japaneseTypedForms", { detail: true }),
    styledToggle("chunks", "candidates", "extra", "segment", j.chunks, "japaneseChunkCandidates", { detail: true }),

    // 辞書と学習 (the page draws its own; these are for All settings, search and AI)
    {
      id: "learn", page: "dictionary", group: "learning", keys: ["learningEnabled"], title: t.dictionary.learn.title,
      render: () => (
        <Row icon="school" title={t.dictionary.learn.title} description={t.dictionary.learn.description}>
          <Toggle label={t.dictionary.learn.title} checked={s.learningEnabled}
            onChange={async (value) => {
              // One switch for everything learned from choices.
              if (await set("learningEnabled", value) && Boolean(s.socialPersonalization) !== value) await set("socialPersonalization", value ? 1 : 0);
            }} />
        </Row>
      ),
    },
    toggle("secret", "dictionary", "learning", "lock", t.dictionary.secret, "secretMode"),
    toggle("private", "dictionary", "learning", "lock", t.dictionary.private, "noLearningInPrivate"),
    {
      id: "encryption", page: "dictionary", group: "learning", keys: [], title: t.dictionary.encryption.title, detail: true,
      render: () => <EncryptionRow encryption={ctx.encryption} runAction={ctx.runAction} setConfirm={ctx.setConfirm} />,
    },

    // キーと入力
    choice("keyboard", "keys", "keyboard", "keyboard_alt", g.keyboard, "keyboardType",
      [[0, g.keyboard.detect], [1, g.keyboard.japanese], [2, g.keyboard.us]]),
    toggle("kanaInput", "keys", "keyboard", "keyboard", j.kanaInput, "japaneseKanaInput", {
      modes: ja, keys: ["japaneseKanaInputKey"],
      sub: () => <Check label={j.kanaInput.key} checked={s.japaneseKanaInputKey} onChange={(value) => set("japaneseKanaInputKey", value)} />,
    }),
    toggle("capsLock", "keys", "keyboard", "keyboard_capslock", j.capsLock, "japaneseIgnoreCapsLock", { modes: ja, detail: true }),
    toggle("numpad", "keys", "keyboard", "dialpad", j.numpad, "japaneseNumpadDirect", { modes: ja, detail: true }),
    {
      id: "romaji", page: "keys", group: "typing", keys: [], title: j.romaji.title, modes: ja,
      render: () => <LinkRow icon="table" title={head(j.romaji.title, ja)} description={j.romaji.description} onClick={ctx.openRomaji} />,
    },
    toggle("brackets", "keys", "typing", "data_object", g.brackets, "bracketPairs"),
    {
      id: "keyList", page: "keys", group: "typing", keys: [], title: t.keysRow,
      render: () => <LinkRow icon="keyboard_command_key" title={t.keysRow} onClick={() => ctx.showKeys("all")} />,
    },
  ];
}

// What is shown: its modes on, its own condition met, and an easter egg
// only once found.
export function visible(entry, ctx) {
  if (entry.show === false) return false;
  if (entry.modes && !entry.modes.some((mode) => ctx.on[mode])) return false;
  if (entry.hidden) return Boolean(entry.keys.some((key) => ctx.settings[key])) || wasFound(entry.hidden);
  return true;
}

export function RowWrap({ entry }) {
  return <div className="row-wrap" id={`row-${entry.id}`}>{entry.render()}</div>;
}

// A catalog page: its groups, with the finer ones under 詳細.
export function CatalogPage({ page, ctx }) {
  const t = ctx.t;
  const entries = catalog(ctx).filter((entry) => entry.page === page && visible(entry, ctx));
  const groups = pageGroups[page];
  const section = (detail) => groups.map((group) => {
    const rows = entries.filter((entry) => entry.group === group && Boolean(entry.detail) === detail);
    if (rows.length === 0) return null;
    return (
      <React.Fragment key={group}>
        <Heading>{t.groups[group]}</Heading>
        {rows.every((entry) => entry.bare)
          ? rows.map((entry) => <RowWrap key={entry.id} entry={entry} />)
          : <Card label={t.groups[group]}>{rows.map((entry) => <RowWrap key={entry.id} entry={entry} />)}</Card>}
      </React.Fragment>
    );
  });
  return (
    <>
      {section(false)}
      {entries.some((entry) => entry.detail) && <Details id={page} title={t.details}>{section(true)}</Details>}
    </>
  );
}

// What AI may change, by key: the setting it belongs to, the values Settings
// offers ([value, name]) and, for numbers, the range the setting takes.
// Downloads, the AI's own settings, lists and the switch order are not here.
export function aiSpecs(ctx) {
  const { t } = ctx;
  const g = t.general;
  const j = t.japanese;
  const a = t.auto;
  const onOff = [[true, t.chat.on], [false, t.chat.off]];
  const entries = Object.fromEntries(catalog(ctx).filter((entry) => !entry.hidden && visible(entry, ctx)).map((entry) => [entry.id, entry]));
  const specs = {};
  const w = t.widths;
  // Keys that are a part of their row, by their own name.
  const titles = {
    japaneseLiveRefine: j.live.refine, modeBadgeOnFocus: g.indicator.onFocus, capitalizedCorrection: a.capitalized.title,
    meaningsInUiLanguage: g.meaningsLanguage.title, dateConversion: g.date.title, symbolConversion: g.symbols.title,
    calculatorEnabled: g.calculator.title, spacelessEnglishAlways: a.spacelessAlways.title, japaneseKanaInputKey: j.kanaInput.key,
    numberConversion: j.numbers.other, japaneseRoughNumbersKanji: j.numbers.rough, japaneseLetterWidth: `${w.title}（${w.letters}）`,
    japaneseSymbolWidth: `${w.title}（${w.symbols}）`, japaneseSpaceWidth: `${w.title}（${w.space}）`, japaneseKanjiNames: j.kanji.names,
  };
  // The mode a key of a linked pair is for.
  const modeOf = {
    japaneseLiveConversion: "japanese", mixLiveConversion: "mixed", japanesePredictionEnabled: "japanese", mixPredictionEnabled: "mixed",
    japaneseFirstPageSymbol: "japanese", mixFirstPageSymbol: "mixed", mixCorrection: "mixed", correctionEnabled: "auto",
  };
  const add = (id, key, standard, extra = {}) => {
    if (entries[id]) specs[key] = { id, title: titles[key] || entries[id].title, mode: modeOf[key], standard, ...extra };
  };
  const width = [[1, j.space.half], [0, j.space.full]];
  for (const [id, keys] of Object.entries({
    modePerApp: ["modePerApp"], indicator: ["modeIndicatorEnabled", "modeBadgeOnFocus"], underline: ["modeUnderlineColors"],
    mixMarker: ["mixSettledMarker"], mixEnglishUnderline: ["mixEnglishUnderline"],
    live: ["japaneseLiveConversion", "mixLiveConversion", "japaneseLiveRefine"], sentenceCheck: ["japaneseSentenceCheck"],
    singleVowel: ["mixSingleVowelKana"], prediction: ["japanesePredictionEnabled", "mixPredictionEnabled"],
    completion: ["completionEnabled"], nextWord: ["nextWordPrediction"], phonetic: ["japanesePhoneticSuggestionsEnabled"],
    slipAutoApply: ["japaneseSlipAutoApply"], englishCorrection: ["mixCorrection", "correctionEnabled", "capitalizedCorrection"],
    doubled: ["doubledWordCheck"], recheck: ["recheckPreviousWord"], spacing: ["japaneseEnglishSpacing"],
    kanjiVariants: ["japaneseKanjiVariants"], period: ["periodOnEnter"], doubleSpace: ["doubleSpacePeriod"], smart: ["smartPunctuation"],
    meanings: ["meaningsEnabled", "meaningsInUiLanguage"], candidateWindow: ["candidateWindowEnabled"],
    special: ["dateConversion", "symbolConversion", "calculatorEnabled"], firstPageSymbol: ["japaneseFirstPageSymbol", "mixFirstPageSymbol"],
    typedForms: ["japaneseTypedForms"], chunks: ["japaneseChunkCandidates"], kanaInput: ["japaneseKanaInput", "japaneseKanaInputKey"], capsLock: ["japaneseIgnoreCapsLock"],
    numpad: ["japaneseNumpadDirect"], brackets: ["bracketPairs"], spaceless: ["spacelessEnglish", "spacelessEnglishAlways"],
    numbers: ["japaneseRoughNumbersKanji", "numberConversion"],
    learn: ["learningEnabled"], secret: ["secretMode"], private: ["noLearningInPrivate"],
    companion: ["desktopCompanion"],
  })) {
    for (const key of keys) add(id, key, onOff);
  }
  if (ctx.japaneseInstalled) {
    add("switchOrder", "japaneseEnabled", onOff, { title: t.modes.find(t.modeNames.japanese) });
    add("switchOrder", "mixedTypingEnabled", onOff, { title: t.modes.find(t.modeNames.mixed) });
  }
  add("switchKey", "toggleKey", [[1, g.switchKey.title], [2, "Ctrl + Space"], [3, "Ctrl + Shift + Space"], [0, g.switchKey.none]]);
  add("language", "uiLanguage", [[0, g.language.system], [1, "English"], [2, "日本語"]]);
  add("spelling", "englishSpelling", a.spelling.options.map((name, value) => [value, name]));
  add("casual", "socialExpressionRange", a.casual.stops.map((name, value) => [value, name]));
  add("punctuation", "japanesePunctuation", [[0, "、。"], [1, "，．"], [2, "，。"], [3, "、．"]]);
  add("numbers", "japaneseNumberStyle", j.numbers.options.map(([name], index) => [index + 1, name]));
  add("widths", "japaneseLetterWidth", width);
  add("widths", "japaneseSymbolWidth", width);
  add("widths", "japaneseSpaceWidth", width);
  add("kanji", "japaneseKanjiRange", [[0, j.kanji.stops[7]], [2, j.kanji.stops[6]], [3, t.chat.byGrade]]);
  add("kanji", "japaneseKanjiGrade", j.kanji.stops.slice(0, 6).map((name, index) => [index + 1, name]));
  add("kanji", "japaneseKanjiNames", onOff);
  add("looks", "candidateWindowStyle", [[0, g.styles.glass], [1, g.styles.simple]]);
  add("rows", "candidateRows", [[0, g.rows.automatic], [5, "5"], [7, "7"], [9, "9"]], { range: candidateRowsRange, unit: g.rows.unit });
  add("keyboard", "keyboardType", [[0, g.keyboard.detect], [1, g.keyboard.japanese], [2, g.keyboard.us]]);
  // Mix's own ways of writing, beside Japanese mode's: each named for its mode.
  if (ctx.on.mixed) {
    for (const [key, mixKey] of Object.entries(mixPairs)) {
      if (!specs[key]) continue;
      specs[mixKey] = { ...specs[key], mode: "mixed" };
      if (ctx.on.japanese) specs[key] = { ...specs[key], mode: "japanese" };
    }
  }
  return specs;
}

// A value as Settings names it.
export function valueName(spec, value, t) {
  const named = spec.standard.find(([standard]) => standard === value);
  if (named) return named[1];
  return spec.unit ? `${value} ${spec.unit}` : String(value);
}

// What may be done with one change AI asks for, at the user's level
// (UserSettings::aiSettingsControl): "apply", "suggest" or "refuse".
export function changeAllowed(spec, value, level) {
  if (!spec) return "refuse";
  const offered = spec.standard.some(([standard]) => standard === value);
  const inRange = Boolean(spec.range) && Number.isInteger(value) && value >= spec.range[0] && value <= spec.range[1];
  if (!offered && !inRange) return "refuse";
  if (level === 0) return "suggest";
  if (level === 1 && !offered) return "suggest";
  return "apply";
}
