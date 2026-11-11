// ホーム: one box. With AI on, a conversation: AI changes settings as asked
// (as far as UserSettings::aiSettingsControl lets it), each change shown
// with a way back. Without AI, it finds settings by name. The modes below.
// すべての設定: every setting on one page, or only those changed.
import React, { useEffect, useRef, useState } from "react";
import { hostRequest } from "./host.js";
import { aiSpecs, catalog, changeAllowed, navPages, valueName, visible } from "./catalog.jsx";
import { keyGroups, ModeBadges, revealHidden } from "./rows.jsx";
import { Busy, Button, Card, Heading, IconButton, Keycaps, LinkRow, ModeIcon, Symbol, Tabs, TextBox, Toggle } from "./ui.jsx";

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

// What AI said, for the next request: its words and the changes made.
function remembered(message, specs, t) {
  const changes = (message.changes || []).filter((change) => change.state === "applied")
    .map((change) => `${change.key}=${JSON.stringify(change.value)}`);
  return changes.length ? `${message.text} (${changes.join(", ")})` : message.text || "";
}

export function HomePage({ ctx, aiReady }) {
  const t = ctx.t;
  const h = t.home;
  const c = t.chat;
  const [query, setQuery] = useState("");
  const { messages, setMessages } = ctx.chat;
  const input = useRef(null);
  const end = useRef(null);
  useEffect(() => { input.current?.focus(); }, []);
  // Newer browsers' scrollIntoView returns a promise: nothing is returned here.
  useEffect(() => { end.current?.scrollIntoView({ block: "nearest" }); }, [messages.length]);
  const needle = query.trim().toLowerCase();
  const found = needle ? findSettings(ctx, needle) : [];
  const level = ctx.settings.aiSettingsControl ?? 1;
  const busy = messages.some((message) => message.status === "asking");

  const update = (index, change) => setMessages((list) => list.map((message, at) => (at === index ? { ...message, ...change(message) } : message)));
  const send = async () => {
    const text = query.trim();
    if (!text || busy) return;
    setQuery("");
    const specs = aiSpecs(ctx);
    const history = [...messages.filter((message) => message.status !== "failed"), { role: "user", text }];
    const at = history.length;
    setMessages([...history, { role: "assistant", status: "asking" }]);
    const response = await hostRequest("settings.chat", {
      language: t.lang,
      level,
      messages: history.map((message) => ({ role: message.role, text: message.role === "user" ? message.text : remembered(message, specs, t) })),
      // The settings AI may name, with their values now and the values they take.
      settings: Object.entries(specs).map(([key, spec]) => ({
        key, setting: spec.title, mode: spec.mode, current: ctx.settings[key], values: spec.standard.map(([value, name]) => ({ value, name })),
        range: spec.range,
      })),
      show: catalog(ctx).filter((entry) => !entry.hidden && visible(entry, ctx)).map((entry) => ({ id: entry.id, setting: entry.title })),
    });
    if (!response.ok) {
      setMessages((list) => list.map((message, index) => (index === at ? { role: "assistant", status: "failed" } : message)));
      return;
    }
    const changes = [];
    for (const { key, value } of response.actions || []) {
      const verdict = changeAllowed(specs[key], value, level);
      const before = ctx.settings[key];
      if (verdict === "apply") {
        if (before === value) continue;
        changes.push({ key, value, before, state: (await ctx.setSetting(key, value)) ? "applied" : "refused" });
      } else {
        changes.push({ key, value, before, state: verdict === "suggest" ? "suggested" : "refused" });
      }
    }
    setMessages((list) => list.map((message, index) => (index === at
      ? { role: "assistant", status: "done", text: response.reply || "", changes, show: response.show || [] } : message)));
  };
  const onKeyDown = (event) => {
    if (event.key === "Escape") setQuery("");
    if (event.key !== "Enter" || event.nativeEvent.isComposing) return;
    if (aiReady) send();
    else if (found[0]) goTo(ctx, found[0]);
  };

  return (
    <>
      {aiReady && messages.length > 0 && (
        <div className="chat" aria-live="polite">
          {messages.map((message, index) => (message.role === "user"
            ? <p key={index} className="bubble-user">{message.text}</p>
            : <Reply key={index} ctx={ctx} message={message} update={(change) => update(index, change)} />))}
          <div ref={end} />
        </div>
      )}

      <TextBox ref={input} className="home-search" size="large" aria-label={aiReady ? c.label : t.search}
        placeholder={aiReady ? (messages.length ? c.next : h.placeholderAi) : t.search}
        value={query} onChange={(event) => setQuery(event.target.value)} onKeyDown={onKeyDown}
        before={<Symbol name={aiReady ? "wand_stars" : "search"} size={20} />}
        after={aiReady && messages.length > 0 ? <IconButton icon="add" label={c.newChat} onClick={() => setMessages([])} /> : undefined} />

      {needle && found.length > 0 && (
        <Card label={t.search} className="home-results">
          {found.map((entry) => (
            <button key={`${entry.page}-${entry.id || entry.title}`} type="button" className="result" onClick={() => goTo(ctx, entry)}>
              <Symbol name="search" size={18} />
              <span className="result-title">{entry.title}{ctx.multi && entry.modes && <ModeBadges ids={entry.modes.filter((m) => ctx.on[m])} />}</span>
              <small>{t.pages[entry.page]}</small>
            </button>
          ))}
        </Card>
      )}
      {needle && found.length === 0 && !aiReady && <Card label={t.search} className="home-results"><p className="result-empty">{t.noResults}</p></Card>}

      <Card label={t.pages.samples}>
        <LinkRow icon="play_circle" title={t.pages.samples} description={t.samples.description} onClick={() => ctx.open("samples")} />
        <LinkRow icon="checklist" title={t.pages.all} description={h.allDescription} onClick={() => ctx.open("all")} />
      </Card>
    </>
  );
}

// One reply: AI's words, the changes it made or suggests, and the settings,
// keys or pages it points to.
function Reply({ ctx, message, update }) {
  const t = ctx.t;
  const c = t.chat;
  if (message.status === "asking") return <div className="bubble-ai"><p className="answer-status"><Busy />{c.thinking}</p></div>;
  if (message.status === "failed") return <div className="bubble-ai"><p className="answer-status">{t.home.failed}</p></div>;
  const specs = aiSpecs(ctx);
  const entries = catalog(ctx).filter((entry) => !entry.hidden && visible(entry, ctx));
  const keys = Object.fromEntries(keyGroups(t, { japaneseKeyboard: ctx.japaneseKeyboard, settings: ctx.settings, japanese: ctx.on.japanese })
    .flatMap(([group, , rows]) => rows.map(([id, label, caps]) => [`${group}.${id}`, [label, caps]])));
  const pages = {
    japaneseWords: ["book", t.dictionary.japaneseWords.title, "dictionary"],
    englishWords: ["book", t.dictionary.englishWords.title, "dictionary"],
    learned: ["history", t.dictionary.learned.title, "dictionary"],
    report: ["feedback", t.about.report.title, "about"],
  };
  const setChange = (index, state) => update((current) => ({
    changes: current.changes.map((change, at) => (at === index ? { ...change, state } : change)),
  }));
  const undo = async (index, change) => { if (await ctx.setSetting(change.key, change.before)) setChange(index, "undone"); };
  const apply = async (index, change) => { if (await ctx.setSetting(change.key, change.value)) setChange(index, "applied"); };
  const shown = (message.show || []).filter((item) =>
    (item.type === "setting" && entries.some((entry) => entry.id === item.id)) || (item.type === "keys" && keys[item.id]) ||
    (item.type === "page" && pages[item.id]));
  return (
    <div className="bubble-ai">
      <p className="reply-text"><Symbol name="wand_stars" size={18} /><span>{message.text || (shown.length ? "" : t.home.nothing)}</span></p>
      {message.changes.length > 0 && (
        <Card label={c.changes} className="changes">
          {message.changes.map((change, index) => {
            const spec = specs[change.key];
            const icon = { applied: "check", suggested: "lightbulb", undone: "undo", refused: "block" }[change.state];
            return (
              <div key={`${change.key}-${index}`} className={`change is-${change.state}`}>
                <Symbol name={icon} size={18} />
                <span className="change-name">{spec ? spec.title : change.key}{spec?.mode && <ModeIcon id={spec.mode} size={14} label={t.modeNames[spec.mode]} />}</span>
                {spec && change.state !== "refused" && (
                  <span className="change-values">{valueName(spec, change.before, t)}<Symbol name="chevron_right" size={14} />{valueName(spec, change.value, t)}</span>
                )}
                {change.state === "refused" && <span className="change-values">{c.refused}</span>}
                {change.state === "applied" && <Button variant="quiet" onClick={() => undo(index, change)}>{t.home.undo}</Button>}
                {change.state === "suggested" && <Button variant="primary" onClick={() => apply(index, change)}>{t.home.apply("")}</Button>}
                {change.state === "undone" && <small className="change-note">{c.undone}</small>}
              </div>
            );
          })}
        </Card>
      )}
      {shown.length > 0 && (
        <Card label={c.related}>
          {shown.map((item) => {
            if (item.type === "keys") {
              const [label, caps] = keys[item.id];
              return <div key={item.id} className="row-wrap"><div className="row-block"><div className="row"><span className="row-icon"><Symbol name="keyboard_command_key" /></span><span className="row-text"><span className="row-title">{label}</span></span><span className="row-control"><Keycaps keys={caps} /></span></div></div></div>;
            }
            if (item.type === "page") {
              const [icon, title, page] = pages[item.id];
              return <div key={item.id} className="row-wrap"><LinkRow icon={icon} title={title} description={t.pages[page]} onClick={() => ctx.open(page)} /></div>;
            }
            const entry = entries.find((one) => one.id === item.id);
            return (
              <div key={item.id} className="row-wrap">
                {entry.render()}
                <div className="answer-foot">
                  <Button variant="quiet" onClick={() => ctx.open(entry.page, entry.id, entry.detail ? entry.page : undefined)}>{t.home.openIn(t.pages[entry.page])}</Button>
                </div>
              </div>
            );
          })}
        </Card>
      )}
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
        <Tabs label={t.pages.all} value={changedOnly} options={[[false, all.every], [true, all.changed(count)]]} onChange={setChangedOnly} />
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
