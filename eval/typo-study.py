"""How much of what slips break TEKITO puts right, in Japanese, Mix and
English Auto.

Each sentence is typed twice: right, and with slips put into its keys
(a neighboring key, a key left out, a key typed twice, two keys swapped).
What TEKITO makes of the keys with slips is compared with what it makes of
the same sentence typed right, so a word it converts differently from the
answer key does not count against slips (見る / 観る).

  python eval/typo-study.py --work <dir> --bin <build dir with tekito_ja_eval and tekito_core_eval>
      --ja-data <installed data dir> --en-data <data dir with en/>
      [--chunks NAME japanese|mix KEYS.tsv]... [--english NAME SENTENCES.txt]...
      [--slips 2] [--seed 1]

KEYS.tsv is what eval/ja/check-chunk-sentences.py writes; SENTENCES.txt is
id, tab, sentence. Per set:

- fixed: sentences that come out as typed right after Space alone;
- one pick: or with each wrong chunk (word) put right by one pick from the
  first nine choices;
- without: sentences that come out right with slip correction off
  (Japanese: tekito_ja_eval --no-slips; English: the words as typed);
- recovered: (fixed - without) / (100% - without), the share of what the
  slips broke that TEKITO puts back;
- changed right text: sentences typed right that slip correction changed
  (Japanese, Mix), or words typed right that Space rewrote (English);
- by where the slip is in its chunk (word): first half or second half.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
from pathlib import Path

NEIGHBORS = {
    "q": "wa", "w": "qes", "e": "wrd", "r": "etf", "t": "ryg", "y": "tuh", "u": "yij", "i": "uok", "o": "ipl",
    "p": "ol", "a": "qsz", "s": "awdz", "d": "sefx", "f": "drgc", "g": "fthv", "h": "gyjb", "j": "hukn",
    "k": "jilm", "l": "kop", "z": "asx", "x": "zdc", "c": "xfv", "v": "cgb", "b": "vhn", "n": "bjm", "m": "nk",
}
KINDS = ("neighbor", "missing", "repeated", "swapped")


def slip(word: str, at: int, kind: str, rng: random.Random) -> str | None:
    """`word` with one slip at letter `at`, or None when that slip cannot be made there."""
    c = word[at]
    if kind == "neighbor":
        if c.lower() not in NEIGHBORS:
            return None
        other = rng.choice(NEIGHBORS[c.lower()])
        return word[:at] + (other.upper() if c.isupper() else other) + word[at + 1:]
    if kind == "missing":
        return word[:at] + word[at + 1:] if len(word) > 2 else None
    if kind == "repeated":
        return word[:at] + c + word[at:]
    if at + 1 < len(word) and word[at + 1].isalpha() and word[at + 1].lower() != c.lower():
        return word[:at] + word[at + 1] + c + word[at + 2:]
    return None


def add_slips(words: list[str], count: int, rng: random.Random) -> tuple[list[str], list[tuple[int, str, str]]] | None:
    """`count` slips in different words when there are that many (letters
    only, not a sentence's first capital): the words and, per slip, (word,
    kind, half)."""
    places = [(w, i) for w, word in enumerate(words) for i, c in enumerate(word)
              if c.isalpha() and not (i == 0 and c.isupper())]
    taken: set[int] = set()
    notes = []
    words = list(words)
    for _ in range(200):
        if len(notes) == count:
            break
        w, at = rng.choice(places)
        if (w in taken and len({p[0] for p in places}) >= count) or at >= len(words[w]):
            continue
        kind = rng.choice(KINDS)
        letters = len(re.sub(r"[^A-Za-z]", "", words[w]))
        changed = slip(words[w], at, kind, rng)
        if changed is None or changed == words[w] or len(re.sub(r"[^A-Za-z]", "", changed)) == 0:
            continue
        words[w] = changed
        taken.add(w)
        notes.append((w, kind, "first" if at < letters / 2 else "second"))
    return (words, notes) if len(notes) == count else None


def read_rows(path: Path) -> list[list[str]]:
    return [line.split("\t") for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def read_chunks(path: Path) -> dict[tuple[str, int], list[tuple[str, list[str]]]]:
    """(id, chunk) -> committed text and choices, from --write-chunks or --write-words."""
    rows: dict = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) == 5 and parts[0].startswith("fresh"):  # tekito_ja_eval
            _, row, chunk, committed, choices = parts
            typed = ""
        elif len(parts) == 5:  # tekito_core_eval: id, word, typed, after, choices
            row, chunk, typed, committed, choices = parts
        else:
            continue
        rows.setdefault(row, []).append((committed, choices.split("\x1f") if choices else [], typed))
    return rows


def same(a: str, b: str) -> bool:
    return a.replace(" ", "") == b.replace(" ", "")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--ja-data", type=Path)
    parser.add_argument("--en-data", type=Path)
    parser.add_argument("--chunks", nargs=3, action="append", default=[], metavar=("NAME", "MODE", "KEYS"))
    parser.add_argument("--english", nargs=2, action="append", default=[], metavar=("NAME", "SENTENCES"))
    parser.add_argument("--slips", type=int, default=2)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--limit", type=int, default=100)
    parser.add_argument("--no-english-engine", action="store_true", help="Mix as before English Auto spelled it")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    def run_ja(keys: Path, out: Path, mode: str, slips: bool) -> None:
        d = args.ja_data
        command = [str(args.bin / "tekito_ja_eval.exe"), "--pack", str(d / "japanese-core"), "--chunks", str(keys),
                   "--romaji", str(d / "japanese-romaji"), "--lm", str(d / "japanese-lm"),
                   "--english-scores", str(d / "frequency" / "word-scores.tsv"),
                   "--loanwords", str(d / "japanese-loanwords"), "--japanese-english", str(d / "japanese-english"),
                   "--write-chunks", str(out)]
        if mode == "japanese":
            command.append("--japanese-mode")
        if not slips:
            command.append("--no-slips")
        env = dict(os.environ)
        if mode == "mix" and args.en_data and not args.no_english_engine:
            # English chunks spelled as English Auto spells them.
            command.append("--english-engine")
            env["TEKITO_DATA_PACK_DIR"] = str(args.en_data)
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL, env=env)

    def run_en(sentences: Path, out: Path) -> None:
        env = dict(os.environ, TEKITO_DATA_PACK_DIR=str(args.en_data))
        subprocess.run([str(args.bin / "tekito_core_eval.exe"), "--typed", str(sentences), "--write-words", str(out)],
                       check=True, env=env, stdout=subprocess.DEVNULL)

    results = []
    for name, mode, keys in args.chunks:
        clean, typo, notes = [], [], {}
        for row in read_rows(Path(keys)):
            if len(clean) == args.limit:
                break
            chunks = row[2].split(" ")
            # Only the letters of a chunk get slips; an English chunk keeps
            # its answer mark (*) and punctuation after it.
            bare = [re.sub(r"[*.,!?]+$", "", c) for c in chunks]
            made = add_slips(bare, args.slips, rng)
            if made is None:
                continue
            words, note = made
            typed = [w + c[len(b):] for w, b, c in zip(words, bare, chunks)]
            clean.append("\t".join(row))
            typo.append("\t".join([row[0], row[1], " ".join(typed), *row[3:]]))
            notes[row[1]] = note
        # tekito_ja_eval reads keys files by "_keys" in the name.
        clean_path, typo_path = args.work / f"{name}_clean_keys.tsv", args.work / f"{name}_typo_keys.tsv"
        clean_path.write_text("\n".join(clean) + "\n", encoding="utf-8", newline="\n")
        typo_path.write_text("\n".join(typo) + "\n", encoding="utf-8", newline="\n")
        outs = {}
        for label, path, slips in (("clean", clean_path, True), ("clean_off", clean_path, False),
                                   ("typo", typo_path, True), ("typo_off", typo_path, False)):
            outs[label] = args.work / f"{name}_{label}.out"
            run_ja(path, outs[label], mode, slips)
        results.append(score(name, notes, *(read_chunks(outs[k]) for k in ("clean", "clean_off", "typo", "typo_off"))))
        write_examples(args.work / f"{name}_misses.txt", notes, typo_path, *(read_chunks(outs[k]) for k in ("clean", "typo")))

    for name, sentences in args.english:
        clean, typo, notes = [], [], {}
        for row in read_rows(Path(sentences)):
            if len(clean) == args.limit:
                break
            text = row[1]
            # Slips go into whole words: split at spaces, keep punctuation.
            tokens = text.split(" ")
            made = add_slips(tokens, args.slips, rng)
            if made is None:
                continue
            words, note = made
            clean.append(f"{row[0]}\t{text}")
            typo.append(f"{row[0]}\t{' '.join(words)}")
            notes[row[0]] = note
        clean_path, typo_path = args.work / f"{name}_clean.txt", args.work / f"{name}_typo.txt"
        clean_path.write_text("\n".join(clean) + "\n", encoding="utf-8", newline="\n")
        typo_path.write_text("\n".join(typo) + "\n", encoding="utf-8", newline="\n")
        clean_out, typo_out = args.work / f"{name}_clean.out", args.work / f"{name}_typo.out"
        run_en(clean_path, clean_out)
        run_en(typo_path, typo_out)
        results.append(score_english(name, notes, clean_path, read_chunks(clean_out), read_chunks(typo_out)))
        write_examples(args.work / f"{name}_misses.txt", notes, typo_path, read_chunks(clean_out), read_chunks(typo_out))

    print(f"{'set':<12}{'rows':>6}{'fixed':>8}{'1 pick':>8}{'without':>9}{'recovered':>11}{'changed':>9}"
          f"{'1st half':>10}{'2nd half':>10}")
    for r in results:
        print(f"{r['name']:<12}{r['rows']:>6}{r['fixed']:>7.1f}%{r['pick']:>7.1f}%{r['without']:>8.1f}%"
              f"{r['recovered']:>10.1f}%{r['changed']:>8.1f}%{r['first']:>9.1f}%{r['second']:>9.1f}%")
    return 0


def sentence_scores(clean: dict, typo: dict, ids: list[str]) -> tuple[int, int, dict]:
    """Sentences that match after Space, or after one pick per chunk; and per id the chunks fixed."""
    fixed = pick = 0
    chunk_fixed = {}
    for row in ids:
        a, b = clean.get(row, []), typo.get(row, [])
        if len(a) != len(b) or not a:
            chunk_fixed[row] = [False] * max(len(a), 1)
            continue
        ok = [same(x[0], y[0]) for x, y in zip(a, b)]
        picked = [o or any(same(x[0], choice) for choice in y[1]) for o, x, y in zip(ok, a, b)]
        fixed += all(ok)
        pick += all(picked)
        chunk_fixed[row] = ok
    return fixed, pick, chunk_fixed


def halves(notes: dict, chunk_fixed: dict) -> tuple[float, float]:
    counts = {"first": [0, 0], "second": [0, 0]}
    for row, note in notes.items():
        for word, _, half in note:
            ok = chunk_fixed.get(row, [])
            counts[half][0] += 1
            counts[half][1] += word < len(ok) and ok[word]
    return tuple(100.0 * c[1] / c[0] if c[0] else 0.0 for c in (counts["first"], counts["second"]))


def score(name, notes, clean, clean_off, typo, typo_off) -> dict:
    ids = list(notes)
    n = len(ids)
    fixed, pick, chunk_fixed = sentence_scores(clean, typo, ids)
    without, _, _ = sentence_scores(clean, typo_off, ids)
    changed = sum(1 for row in ids
                  if any(not same(x[0], y[0]) for x, y in zip(clean.get(row, []), clean_off.get(row, []))))
    first, second = halves(notes, chunk_fixed)
    base = 100.0 * without / n
    return {"name": name, "rows": n, "fixed": 100.0 * fixed / n, "pick": 100.0 * pick / n, "without": base,
            "recovered": (100.0 * fixed / n - base) / (100.0 - base) * 100.0 if base < 100 else 0.0,
            "changed": 100.0 * changed / n, "first": first, "second": second}


def score_english(name, notes, clean_path, clean, typo) -> dict:
    ids = list(notes)
    n = len(ids)
    fixed, pick, chunk_fixed = sentence_scores(clean, typo, ids)
    # Without correction: the words as typed, against the clean run.
    typed = {}
    for line in clean_path.read_text(encoding="utf-8").splitlines():
        row, text = line.split("\t", 1)
        typed[row] = text
    # Without correction every sentence keeps its slips.
    without = 0
    # Words typed right that Space rewrote, in the clean run.
    words = changed_words = 0
    for row in ids:
        for committed, _, word in clean.get(row, []):
            words += 1
            changed_words += committed != word
    # The slips' words counted as tekito_core_eval counts words (follow-up is two).
    word_notes = {}
    for row, note in notes.items():
        tokens = typed[row].split(" ")
        word_notes[row] = [(len(re.findall(r"[A-Za-z']+", " ".join(tokens[:w]))), kind, half) for w, kind, half in note]
    first, second = halves(word_notes, chunk_fixed)
    base = 100.0 * without / n
    return {"name": name, "rows": n, "fixed": 100.0 * fixed / n, "pick": 100.0 * pick / n, "without": base,
            "recovered": (100.0 * fixed / n - base) / (100.0 - base) * 100.0 if base < 100 else 0.0,
            "changed": 100.0 * changed_words / words if words else 0.0, "first": first, "second": second}


def write_examples(path: Path, notes: dict, typed_path: Path, clean: dict, typo: dict) -> None:
    typed = {}
    for parts in read_rows(typed_path):
        typed[parts[1] if parts[0].startswith("fresh") else parts[0]] = parts[2] if parts[0].startswith("fresh") else parts[1]
    lines = []
    for row, note in notes.items():
        a, b = clean.get(row, []), typo.get(row, [])
        if len(a) == len(b) and all(same(x[0], y[0]) for x, y in zip(a, b)):
            continue
        lines.append(f"{row}\t{typed.get(row, '')}\n  want: {''.join(x[0] for x in a)}\n  got:  {''.join(y[0] for y in b)}"
                     f"\n  slips: {note}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
