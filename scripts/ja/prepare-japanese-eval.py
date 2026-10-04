#!/usr/bin/env python3
"""Builds the Japanese conversion eval corpus from two public sets.

- Mozc's evaluation.tsv (BSD-3-Clause): the rows whose first conversion
  must be a given text ("Conversion Expected").
- AJIMEE-Bench (azooKey; the JWTD v2 based items are CC BY-SA 3.0): 200
  readings with every acceptable conversion, half of them with the text
  before them.

The sources are downloaded at pinned commits into .cache/tekito-data and
flattened into eval/generated/japanese_eval.tsv (not committed):

  source <TAB> id <TAB> context <TAB> reading <TAB> expected [<TAB> expected ...]

Readings are hiragana, as TEKITO's composer gives them to the converter.

It also writes eval/generated/japanese_eval_keys.tsv, the same sentences as
the romaji keys a person would type (from the japanese-romaji pack):

  source <TAB> id <TAB> keys <TAB> expected [<TAB> expected ...]

And eval/generated/japanese_eval_typo_keys.tsv: the same sentences with one
slip in the keys (a neighboring key, a key dropped, an extra neighboring
key, or two keys swapped), chosen deterministically; the expected text is
what was meant.

With --phrases, it reads eval/generated/japanese_eval_phrase_keys.tsv (the
phrases of the sentences that convert right, written by
`tekito_ja_eval --dump-phrases`) and writes
eval/generated/japanese_eval_phrase_typo_keys.tsv: each phrase of four
letters or more with one slip, for typing and converting phrase by phrase.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import urllib.request
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from datapacks import DATA, manifests, pack_dir  # noqa: E402
CACHE = ROOT / ".cache" / "tekito-data"
OUTPUT = ROOT / "eval" / "generated" / "japanese_eval.tsv"
KEYS_OUTPUT = ROOT / "eval" / "generated" / "japanese_eval_keys.tsv"
TYPO_OUTPUT = ROOT / "eval" / "generated" / "japanese_eval_typo_keys.tsv"
PHRASES = ROOT / "eval" / "generated" / "japanese_eval_phrase_keys.tsv"
PHRASE_TYPO_OUTPUT = ROOT / "eval" / "generated" / "japanese_eval_phrase_typo_keys.tsv"
QWERTY_ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]

MOZC_COMMIT = "b9c3fcbd6d76b19649ef572324fa9da2559bc18e"
AJIMEE_COMMIT = "401666cd56d1a570c2021798b64b6da4396bfd45"
SOURCES = {
    "mozc": (CACHE / f"mozc-{MOZC_COMMIT}" / "dictionary_oss" / "evaluation.tsv",
             f"https://raw.githubusercontent.com/google/mozc/{MOZC_COMMIT}/src/data/dictionary_oss/evaluation.tsv"),
    "ajimee": (CACHE / f"ajimee-{AJIMEE_COMMIT[:8]}" / "evaluation_items.json",
               f"https://raw.githubusercontent.com/azooKey/AJIMEE-Bench/{AJIMEE_COMMIT}/JWTD_v2/v1/evaluation_items.json"),
}


def fetch(name: str) -> Path:
    path, url = SOURCES[name]
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        with urllib.request.urlopen(url) as response:
            path.write_bytes(response.read())
    return path


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def clean(text: str) -> str:
    return text.replace("\t", " ").replace("\n", " ").replace("\r", " ").strip()


def mozc_rows():
    for number, line in enumerate(fetch("mozc").read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("\t")
        if line.startswith("#") or len(fields) < 5 or fields[3] != "Conversion Expected":
            continue
        yield "mozc", str(number), "", fields[1], [fields[4]]


def ajimee_rows():
    for item in json.loads(fetch("ajimee").read_text(encoding="utf-8")):
        yield ("ajimee", item["index"], item.get("context_text", ""), hiragana(item["input"]),
               item["expected_output"])


def romaji_table() -> dict[str, str]:
    """Kana -> the keys that type it: the shortest, the usual spelling first."""
    keys_for: dict[str, str] = {}
    for line in (pack_dir("japanese-romaji") / "romaji.tsv").read_text(encoding="utf-8").splitlines():
        keys, output, pending = line.split("\t")
        if pending:
            continue
        current = keys_for.get(output)
        rank = (len(keys), keys[0] in "clqx", keys)
        if current is None or rank < (len(current), current[0] in "clqx", current):
            keys_for[output] = keys
    return keys_for


def romanize(reading: str, keys_for: dict[str, str]) -> str | None:
    out: list[str] = []
    double_next = False
    i = 0
    while i < len(reading):
        ch = reading[i]
        if ch == "っ" and i + 1 < len(reading):
            double_next = True
            i += 1
            continue
        if ch == "ん":
            out.append("nn")
            i += 1
            continue
        for length in (2, 1):
            part = reading[i:i + length]
            keys = keys_for.get(part)
            if keys and len(part) == length:
                if double_next:
                    if not keys[0].isalpha() or keys[0] in "aiueon":
                        return None
                    keys = keys[0] + keys
                    double_next = False
                out.append(keys)
                i += length
                break
        else:
            return None
    return None if double_next else "".join(out)


def qwerty_neighbors(key: str) -> str:
    """The same neighbors as TypoModel's AreQwertyNeighbors."""
    for row, letters in enumerate(QWERTY_ROWS):
        column = letters.find(key)
        if column < 0:
            continue
        near = [letters[c] for c in (column - 1, column + 1) if 0 <= c < len(letters)]
        for other in (row - 1, row + 1):
            if 0 <= other < len(QWERTY_ROWS):
                near += [QWERTY_ROWS[other][c] for c in (column - 1, column, column + 1)
                         if 0 <= c < len(QWERTY_ROWS[other])]
        return "".join(near)
    return ""


def typo(keys: str, seed: str, shortest: int = 6) -> tuple[str, str] | None:
    """One slip in the keys, the same every run."""
    digest = hashlib.sha1(seed.encode("utf-8")).digest()
    letters = [i for i, c in enumerate(keys) if c.isalpha()]
    if len(letters) < shortest:
        return None
    at = letters[digest[0] % len(letters)]
    kind = ("neighbor", "neighbor", "dropped", "extra", "swapped")[digest[1] % 5]
    near = qwerty_neighbors(keys[at])
    if kind == "neighbor" and near:
        return kind, keys[:at] + near[digest[2] % len(near)] + keys[at + 1:]
    if kind == "dropped":
        return kind, keys[:at] + keys[at + 1:]
    if kind == "extra" and near:
        return kind, keys[:at + 1] + near[digest[2] % len(near)] + keys[at + 1:]
    if kind == "swapped" and at + 1 < len(keys) and keys[at] != keys[at + 1] and keys[at + 1].isalpha():
        return kind, keys[:at] + keys[at + 1] + keys[at] + keys[at + 2:]
    return None


def write_phrase_typos() -> None:
    count = 0
    with PHRASE_TYPO_OUTPUT.open("w", encoding="utf-8", newline="\n") as out:
        for line in PHRASES.read_text(encoding="utf-8").splitlines():
            source, row_id, keys, text = line.split("\t")[:4]
            slip = typo(keys, f"{source}:{row_id}", shortest=4)
            if slip:
                kind, slipped = slip
                out.write(f"{source}-{kind}\t{row_id}\t{slipped}\t{text}\n")
                count += 1
    print(f"wrote {count} rows to {PHRASE_TYPO_OUTPUT}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--phrases", action="store_true",
                        help="write the phrase typo set from japanese_eval_phrase_keys.tsv")
    args = parser.parse_args()
    if args.phrases:
        write_phrase_typos()
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with args.output.open("w", encoding="utf-8", newline="\n") as out:
        for rows in (mozc_rows(), ajimee_rows()):
            for source, row_id, context, reading, expected in rows:
                fields = [source, row_id, clean(context), clean(reading)] + [clean(e) for e in expected]
                out.write("\t".join(fields) + "\n")
                count += 1
    print(f"wrote {count} rows to {args.output}")

    keys_for = romaji_table()
    keyed = 0
    typos = 0
    with KEYS_OUTPUT.open("w", encoding="utf-8", newline="\n") as out, \
            TYPO_OUTPUT.open("w", encoding="utf-8", newline="\n") as typo_out:
        for rows in (mozc_rows(), ajimee_rows()):
            for source, row_id, _, reading, expected in rows:
                keys = romanize(clean(reading), keys_for)
                if keys:
                    out.write("\t".join([source, row_id, keys] + [clean(e) for e in expected]) + "\n")
                    keyed += 1
                    slip = typo(keys, f"{source}:{row_id}")
                    if slip:
                        kind, slipped = slip
                        typo_out.write("\t".join([f"typo-{kind}", f"{source}-{row_id}", slipped] +
                                                  [clean(e) for e in expected]) + "\n")
                        typos += 1
    print(f"wrote {keyed} rows to {KEYS_OUTPUT}")
    print(f"wrote {typos} rows to {TYPO_OUTPUT}")


if __name__ == "__main__":
    main()
