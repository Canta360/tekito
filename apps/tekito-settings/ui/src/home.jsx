// ホーム: one box to find a setting, or to ask AI when AI is on; the modes
// below it. すべての設定: every setting on one page, or only those changed.
import React, { useEffect, useRef, useState } from "react";
import { hostRequest } from "./host.js";
import { catalog, navPages, visible } from "./catalog.jsx";
import { keyGroups, ModeBadges, revealHidden } from "./rows.jsx";
import { Button, Card, Heading, Keycaps, LinkRow, ModeIcon, Segmented, Symbol, Toggle } from "./ui.jsx";

// Settings outside the catalog that the box also finds: [page, name].
function otherEntries(t, japaneseInstalled) {
  const d = t.dictionary;
  const a = t.about;
  return [
    ...(japaneseInstalled ? [["dictionary", d.japaneseWords.title]] : []), ["dictionary", d.englishWords.title], ["dictionary", d.files.title],
    ["dictionary", d.learn.title], ["dictionary", d.learned.title], ["dictionary", d.secret.title], ["dictionary", d.private.title],
    ["dictionary", d.encryption.title, "dictionary"],
    ...(japaneseInstalled ? [["about", a.report.title]] : []), ["about", a.bug.title], ["about", a.feature.title], ["about", a.releases.title],
    ["about", a.diagnostics], ["about", a.license], ["samples", t.pages.samples],
  ].map(([page, title, details]) => ({ page, title, details }));
}

// Settings whose name holds the words typed; an easter egg by its own names only.
function findSettings(ctx, needle) {
  const entries = catalog(ctx);
  const eggs = entries.filter((entry) => entry.hidden && entry.find.includes(needle));
  const named = entries.filter((entry) => !entry.hidden && visible(entry, ctx) &&
    [entry.title, ...(entry.find || [])].some((name) => String(name).toLowerCase().includes(needle)));
  const others = otherEntries(ctx.t, ctx.japaneseInstalled).filter((entry) => entry.title.toLowerCase().includes(needle));
  return [...eggs, ...named, ...others].slice(0, 8);
}

// Where a found setting is: its page, and 詳細 opened when it is there.
function goTo(ctx, entry) {
  if (entry.hidden) revealHidden(entry.hidden);
  ctx.open(entry.page, entry.id, entry.detail ? entry.page : entry.details);
}

export function HomePage({ ctx, mode, changeMode, aiReady }) {
  const t = ctx.t;
  const h = t.home;
  const [query, setQuery] = useState("");
  const [answer, setAnswer] = useState(null);
  const input = useRef(null);
  useEffect(() => input.current?.focus(), []);
  const needle = query.trim().toLowerCase();
  const found = needle ? findSettings(ctx, needle) : [];
  const asked = answer && answer.query === query.trim();
  const ask = async () => {
    const asking = query.trim();
    if (!asking) return;
    setAnswer({ query: asking, status: "asking" });
    // The model may name only these settings, by id.
    const settings = catalog(ctx).filter((entry) => !entry.hidden && visible(entry, ctx))
      .map((entry) => ({ id: entry.id, page: entry.page, title: entry.title }));
    const response = await hostRequest("settings.ask", { query: asking, language: t.lang, settings });
    setAnswer(response.ok ? { query: asking, status: "done", items: response.answers || [] } : { query: asking, status: "failed" });
  };
  const onKeyDown = (event) => {
    if (event.key === "Escape") { setQuery(""); setAnswer(null); }
    if (event.key !== "Enter") return;
    if (aiReady) ask();
    else if (found[0]) goTo(ctx, found[0]);
  };

  const japanese = ctx.on.japanese;
  const mixed = ctx.on.mixed;
  const modes = ctx.japaneseInstalled ? ["japanese", "mixed", "auto", "direct"] : ["auto", "direct"];
  const switchable = { japanese: "japaneseEnabled", mixed: "mixedTypingEnabled" };
  const modeOn = { japanese, mixed, auto: true, direct: true };

  return (
    <>
      <div className="home-search">
        <Symbol name={aiReady ? "wand_stars" : "search"} size={20} />
        <input ref={input} aria-label={aiReady ? h.askLabel : t.search} placeholder={aiReady ? h.placeholderAi : t.search}
          value={query} onChange={(event) => setQuery(event.target.value)} onKeyDown={onKeyDown} />
      </div>

      {needle && !asked && (
        <Card label={t.search} className="home-results">
          {aiReady && (
            <button type="button" className="result is-ask" onClick={ask}>
              <Symbol name="wand_stars" size={18} /><span className="result-title">{h.ask(query.trim())}</span>
            </button>
          )}
          {found.map((entry) => (
            <button key={`${entry.page}-${entry.id || entry.title}`} type="button" className="result" onClick={() => goTo(ctx, entry)}>
              <Symbol name="search" size={18} />
              <span className="result-title">{entry.title}{ctx.multi && entry.modes && <ModeBadges ids={entry.modes.filter((m) => ctx.on[m])} />}</span>
              <small>{t.pages[entry.page]}</small>
            </button>
          ))}
          {found.length === 0 && !aiReady && <p className="result-empty">{t.noResults}</p>}
        </Card>
      )}

      {asked && <Answer ctx={ctx} answer={answer} />}

      <Heading>{h.modes}</Heading>
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
                  <Toggle label={t.modeNames[id]} checked={modeOn[id]} onChange={(value) => ctx.setSetting(switchable[id], value)} />
                </span>
              )}
            </span>
            <small>{mode === id ? t.inUse : ""}</small>
          </div>
        ))}
      </div>

      <Card label={t.pages.samples}>
        <LinkRow icon="play_circle" title={t.pages.samples} description={t.samples.description} onClick={() => ctx.open("samples")} />
        <LinkRow icon="checklist" title={t.pages.all} description={h.allDescription} onClick={() => ctx.open("all")} />
      </Card>
    </>
  );
}

// AI's answer: the settings it names, as their own rows (with a change it
// suggests, to apply or undo), keys, or a page that helps.
function Answer({ ctx, answer }) {
  const t = ctx.t;
  const h = t.home;
  if (answer.status === "asking") {
    return <Card label={h.askLabel} className="answer"><p className="answer-status"><span className="progress" />{h.asking}</p></Card>;
  }
  if (answer.status === "failed") return <Card label={h.askLabel} className="answer"><p className="answer-status">{h.failed}</p></Card>;
  const entries = catalog(ctx).filter((entry) => !entry.hidden && visible(entry, ctx));
  const keys = Object.fromEntries(keyGroups(t, { japaneseKeyboard: ctx.japaneseKeyboard, settings: ctx.settings, japanese: ctx.on.japanese })
    .flatMap(([group, , rows]) => rows.map(([id, label, caps]) => [`${group}.${id}`, [label, caps]])));
  const pages = {
    japaneseWords: ["book", t.dictionary.japaneseWords.title, "dictionary"],
    englishWords: ["book", t.dictionary.englishWords.title, "dictionary"],
    learned: ["history", t.dictionary.learned.title, "dictionary"],
    report: ["feedback", t.about.report.title, "about"],
  };
  const items = (answer.items || []).filter((item) =>
    (item.type === "setting" && entries.some((entry) => entry.id === item.id)) || (item.type === "keys" && keys[item.id]) ||
    (item.type === "page" && pages[item.id]));
  if (items.length === 0) return <Card label={h.askLabel} className="answer"><p className="answer-status">{h.nothing}</p></Card>;
  return (
    <Card label={h.askLabel} className="answer">
      {items.map((item) => {
        if (item.type === "keys") {
          const [label, caps] = keys[item.id];
          return <div key={item.id} className="row-wrap"><div className="row-block"><div className="row"><span className="row-icon"><Symbol name="keyboard_command_key" /></span><span className="row-text"><span className="row-title">{label}</span></span><span className="row-control"><Keycaps keys={caps} /></span></div></div></div>;
        }
        if (item.type === "page") {
          const [icon, title, page] = pages[item.id];
          return <div key={item.id} className="row-wrap"><LinkRow icon={icon} title={title} description={t.pages[page]} onClick={() => ctx.open(page)} /></div>;
        }
        const entry = entries.find((one) => one.id === item.id);
        return <AnswerSetting key={item.id} ctx={ctx} entry={entry} change={item.change} />;
      })}
    </Card>
  );
}

function AnswerSetting({ ctx, entry, change }) {
  const t = ctx.t;
  const h = t.home;
  const [before, setBefore] = useState(null);
  const applied = before !== null;
  // A change to one of two linked switches changes both while they agree.
  const linked = entry.linkedKeys || [];
  const together = linked.includes(change?.key) && linked.every((key) => ctx.settings[key] === ctx.settings[change.key]);
  const changeKeys = together ? linked : [change?.key];
  const apply = async () => {
    const previous = Object.fromEntries(changeKeys.map((key) => [key, ctx.settings[key]]));
    for (const key of changeKeys) await ctx.setSetting(key, change.value);
    setBefore(previous);
  };
  const undo = async () => {
    for (const [key, value] of Object.entries(before)) await ctx.setSetting(key, value);
    setBefore(null);
  };
  const pending = change && !applied && ctx.settings[change.key] !== change.value;
  return (
    <div className="row-wrap">
      {entry.render()}
      <div className="answer-foot">
        {pending && <Button variant="primary" onClick={apply}>{h.apply(change.label)}</Button>}
        {applied && <Button onClick={undo}>{h.undo}</Button>}
        <Button variant="quiet" onClick={() => ctx.open(entry.page, entry.id, entry.detail ? entry.page : undefined)}>{h.openIn(t.pages[entry.page])}</Button>
      </div>
    </div>
  );
}

// すべての設定: every page's settings, the changed ones alone if asked, each
// with its way back to how TEKITO comes.
export function AllSettingsPage({ ctx, defaults }) {
  const t = ctx.t;
  const all = t.all;
  const [changedOnly, setChangedOnly] = useState(false);
  const entries = catalog(ctx).filter((entry) => visible(entry, ctx) && !entry.bare);
  const resettable = (entry) => entry.keys.filter((key) => key in defaults && !Array.isArray(defaults[key]));
  const changed = (entry) => entry.keys.some((key) => key in defaults && JSON.stringify(ctx.settings[key]) !== JSON.stringify(defaults[key]));
  const shown = changedOnly ? entries.filter(changed) : entries;
  const reset = async (entry) => {
    for (const key of resettable(entry)) {
      if (JSON.stringify(ctx.settings[key]) !== JSON.stringify(defaults[key])) await ctx.setSetting(key, defaults[key]);
    }
  };
  const count = entries.filter(changed).length;
  return (
    <>
      <div className="filter-bar">
        <Segmented label={t.pages.all} value={changedOnly} options={[[false, all.every], [true, all.changed(count)]]} onChange={setChangedOnly} />
      </div>
      {navPages.filter((page) => shown.some((entry) => entry.page === page)).map((page) => (
        <React.Fragment key={page}>
          <Heading><button type="button" className="heading-link" onClick={() => ctx.open(page)}>{t.pages[page]}<Symbol name="chevron_right" size={16} /></button></Heading>
          <Card label={t.pages[page]}>
            {shown.filter((entry) => entry.page === page).map((entry) => (
              <div key={entry.id} className="row-wrap" id={`row-${entry.id}`}>
                {entry.render()}
                {changedOnly && resettable(entry).length > 0 && (
                  <div className="answer-foot"><Button variant="quiet" icon="restart_alt" onClick={() => reset(entry)}>{all.reset}</Button></div>
                )}
              </div>
            ))}
          </Card>
        </React.Fragment>
      ))}
      {shown.length === 0 && <Card label={t.pages.all}><p className="answer-status">{all.none}</p></Card>}
    </>
  );
}

