# Offline evaluation harness

Measures the real `CandidateEngine` / `AutoApplyPolicy` against two corpora
already in `data/`:

- `data/qwerty-typo-catalog` (about 23,800 rows: synthetic QWERTY slips of
  the 5,000 most common words, the same words typed correctly, and the chat
  abbreviations from the slang pack, which must stay as typed; each row is
  assigned to a `test` / `validation` / `train` split; built by
  `scripts/build-expression-packs.py`).
- `data/wikipedia-common-misspellings` (4,499 real human misspelling ->
  correction pairs, CC-BY-SA-4.0, no context; treated as an eval-only `test`
  set).

Neither corpus is read by the harness directly: `scripts/prepare-eval-corpus.py`
flattens both into one TSV (`eval/generated/eval_corpus.tsv`, gitignored,
regenerate any time) that `tekito_core_eval` reads.

## What it measures

- **Top-1 / top-9 accuracy**: for rows whose `expected_action` is
  `auto_apply_eligible`, does `expected_token` appear at rank 0 / within the
  first 9 entries of `CandidateEngine::Convert(...).candidates`?
- **False correction rate**: for rows whose `expected_action` is `keep_raw` or
  `suggest_only` (already-correct words, proper nouns, URLs, user-dictionary
  entries, valid-word confusions such as too/to), does
  `AutoApplyPolicy::SelectForBoundary(rawText, candidates)` return a selection
  at all? `SelectForBoundary` only ever selects a candidate whose text differs
  from the raw input, so any selection here is by definition an unwanted
  silent rewrite of a word that should have been left alone.
- **Latency**: `full_word_latency_us_*` is one `Convert()` sample per row.
  `per_keystroke_latency_us_*` retypes the raw token one character at a time
  (capped at `--latency-sample` rows, default 3000) and times every
  incremental `Convert()` call, which is closer to what the TSF text service
  actually does on each keystroke.

This only exercises the candidate engine directly, bypassing the text
service, so it says nothing about where words begin and end (URLs, email
addresses and so on); it only tells you what the engine does once a string
reaches it.

## Running it

```powershell
.\scripts\run-eval.ps1
```

This regenerates the TSV, configures/builds `tekito_core_eval` (Release), and
runs it against the `test` split. Pass `-Split validation`, `-Split train`, or
`-Split all` to run a different slice, `-LatencySample 0` to skip the
per-keystroke pass, or `-Corpus <path>` for a custom TSV. See
`scripts/prepare-eval-corpus.py --help` for the corpus generator's own
options.

## Interpreting results

Re-run after each change to candidate generation/ranking and compare against
the previous run's `overall_top1_rate` / `overall_top9_rate` /
`overall_false_correction_rate` / latency percentiles -- none of the four
should regress. The per-category breakdown (`accuracy_category` /
`safety_category` rows) shows which failure mode dominates (missing from the
candidate list at all vs. ranked too low vs. wrongly auto-applied) so the
next fix can be targeted.

### English Auto on real text

`--sentences <eng_news_*-sentences.txt>` types each word of real, correctly
typed sentences (Leipzig, CC BY 4.0) the way the text service does (128
characters of context before it) and lets `InputStateMachine::OnSpace()`
decide, so every word Space rewrites is a false correction. It runs TEKITO
as it is, keeping a word once its correction is undone (user dictionary),
and deciding at the next word; each also gets one real word a letter away
per sentence to see how many such slips Space puts right. `--corpus` adds a
check that the kept words do not stop the corpus's typo corrections, and
`--probe "<sentence>"` prints what Space does to each word and why.

Unlike `overall_false_correction_rate`, which asks `AutoApplyPolicy` about
words without context, this follows what Space actually does.

## Japanese conversion

`tekito_ja_eval` measures kana-kanji conversion with the `japanese-core` pack
on two public sets, which `scripts/prepare-japanese-eval.py` downloads at
pinned commits and flattens into `eval/generated/japanese_eval.tsv`:

- Mozc's `evaluation.tsv` (BSD-3-Clause): the rows whose first conversion
  must be a given text.
- AJIMEE-Bench (azooKey; the items built on the Japanese Wikipedia Typo
  Dataset are CC BY-SA 3.0): 200 readings with every acceptable conversion.

```powershell
.\scripts\prepare-japanese-packs.ps1   # once, builds data\japanese-core
.\scripts\run-ja-eval.ps1 -ShowMisses 20
```

Per source it reports `top1` (the first conversion is acceptable),
`top10_per_phrase` (an acceptable text can be made from each phrase's first
ten candidates), `cer` (character error rate against the closest acceptable
text) and conversion time. None of them should regress.

The script also writes the same sentences as the romaji keys a person types
(`japanese_eval_keys.tsv`) and with one slip in the keys (`japanese_eval_typo_keys.tsv`: a
neighboring key, a key dropped, an extra key, two keys swapped). These run
through the composer, as TEKITO converts on Space:

```powershell
$keys = @("--pack", "data\japanese-core", "--romaji", "data\japanese-romaji")
.\out\build\eval\Release\tekito_ja_eval.exe @keys --keys-corpus eval\generated\japanese_eval_keys.tsv
.\out\build\eval\Release\tekito_ja_eval.exe @keys --keys-corpus eval\generated\japanese_eval_typo_keys.tsv
```

`--typo N` changes what a corrected slip costs (0 turns correction off).
Correct keys must convert as well as with correction off; the typo set
shows how many slips come out as meant (37.6% with the default, against
61% for the same sentences typed correctly). `in_list` counts what was
meant among the first nine candidates of one phrase, as the second Space
shows them with the slips undone there (64.3% of the typo set), and
`list_us` is how long opening that list takes.

Typing phrase by phrase (a word or a phrase, then Space) is measured with
the phrases of the sentences that convert right:

```powershell
.\out\build\eval\Release\tekito_ja_eval.exe @keys --keys-corpus eval\generated\japanese_eval_keys.tsv `
    --dump-phrases eval\generated\japanese_eval_phrase_keys.tsv
python scripts\prepare-japanese-eval.py --phrases   # writes japanese_eval_phrase_typo_keys.tsv
.\out\build\eval\Release\tekito_ja_eval.exe @keys --keys-corpus eval\generated\japanese_eval_phrase_keys.tsv
.\out\build\eval\Release\tekito_ja_eval.exe @keys --keys-corpus eval\generated\japanese_eval_phrase_typo_keys.tsv
```

The phrases alone convert as in their sentence 97.3% of the time; with one
slip, what was meant is the first choice for 55.9% and one pick away for
91.2%.

`--by-phrase` types each sentence of a keys file phrase by phrase instead,
converting and committing each phrase, so the committed text leads into the
next conversion; `--no-context` starts every phrase afresh. On
`japanese_eval_keys.tsv`, 58.9% of the sentences come out right without the
context and 61.3% with it (61.5% when converted whole).

For a larger set, `scripts/prepare-japanese-jsut-eval.py` writes
`japanese_eval_jsut.tsv` (and `_keys`): about 4,000 of the 5,000 JSUT
basic5000 sentences, their typed readings recovered from the spoken ones
through the Mozc dictionary (the text is CC BY-SA / CC BY and is not
committed). Only one writing counts as right there, so `cer` says more than
`top1`.

`--lm data\japanese-lm` adds the language model, with `--lm-scale`,
`--lm-penalty`, `--lm-threshold` and `--topic-scale` to tune it
(JapaneseConverter::ModelWeights has the defaults). With it the JSUT
sentences go from 36.9% to 38.9% (cer 6.8% to 6.5%) and the Mozc and
AJIMEE ones from 61.2% to 62.0% (cer 7.0% to 6.4%); conversion takes about
twice as long, a few milliseconds.

Typed phrase by phrase with the language model, the content words committed
before count as the sentence's too: 63.0% of the Mozc and AJIMEE sentences
and 37.7% of the JSUT ones come out right that way.
