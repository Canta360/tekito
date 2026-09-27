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
the romaji keys a person would type (from the japanese-romaji pack), plus
sentences with a common English noun typed among the romaji
("kyouha" + "meeting" + "gaarimasu"):

  source <TAB> id <TAB> keys <TAB> expected [<TAB> expected ...]
"""

from __future__ import annotations

import argparse
import json
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "tekito-data"
OUTPUT = ROOT / "eval" / "generated" / "japanese_eval.tsv"
KEYS_OUTPUT = ROOT / "eval" / "generated" / "japanese_eval_keys.tsv"
DATA = ROOT / "data"

# Japanese around the English word: keys before, their text, keys after,
# their text. The texts are what the converter gives for them alone.
MIXED_TEMPLATES = [
    ("kyouha", "今日は", "gaarimasu", "があります"),
    ("", "", "wotsukaimasu", "を使います"),
    ("atarashii", "新しい", "desu", "です"),
    ("", "", "nosettei", "の設定"),
    ("watashino", "私の", "hakoredesu", "はこれです"),
    ("", "", "gasukidesu", "が好きです"),
]
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
    for line in (DATA / "japanese-romaji" / "romaji.tsv").read_text(encoding="utf-8").splitlines():
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


def english_nouns(limit: int, keys_for: dict[str, str]) -> list[str]:
    """Common English nouns that romaji does not read, 4 to 9 letters."""
    nouns = set()
    for line in (DATA / "dictionary-display" / "entries.tsv").read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) > 3 and fields[3] == "noun" and fields[0].startswith("external:"):
            nouns.add(fields[0][len("external:"):])
    scored = []
    for line in (DATA / "japanese-english-words" / "words.tsv").read_text(encoding="utf-8").splitlines():
        word, score = line.split("\t")
        if 4 <= len(word) <= 9 and word.isalpha() and word in nouns:
            scored.append((-float(score), word))
    scored.sort()
    readable = {line.split("	")[0] for line in
                (DATA / "japanese-romaji" / "romaji.tsv").read_text(encoding="utf-8").splitlines()}
    words = []
    for _, word in scored:
        # A word that is also good romaji is ambiguous; keep the test clear.
        if reads_as_romaji(word, readable):
            continue
        words.append(word)
        if len(words) >= limit:
            break
    return words


def reads_as_romaji(word: str, keys: set[str]) -> bool:
    i = 0
    while i < len(word):
        for length in (4, 3, 2, 1):
            if word[i:i + length] in keys:
                i += length
                break
        else:
            if word[i] == word[i + 1:i + 2] and word[i] not in "aiueon":
                i += 1
                continue
            return False
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
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
    with KEYS_OUTPUT.open("w", encoding="utf-8", newline="\n") as out:
        for rows in (mozc_rows(), ajimee_rows()):
            for source, row_id, _, reading, expected in rows:
                keys = romanize(clean(reading), keys_for)
                if keys:
                    out.write("\t".join([source, row_id, keys] + [clean(e) for e in expected]) + "\n")
                    keyed += 1
        for word in english_nouns(60, keys_for):
            for number, (before, before_text, after, after_text) in enumerate(MIXED_TEMPLATES):
                out.write(f"mixed\t{word}-{number}\t{before}{word}{after}\t{before_text}{word}{after_text}\n")
                keyed += 1
    print(f"wrote {keyed} rows to {KEYS_OUTPUT}")


if __name__ == "__main__":
    main()
