"""Lists proper nouns from Wikidata (CC0) with their readings, for
build-japanese-packs.py --names-words: items with a name in kana (P1814)
and their Japanese label, and items whose Japanese label is all katakana
(foreign places and people: ボストン, ドナルド・トランプ), read as written.
Written out as build-japanese-wikipedia-words.py writes:

  reading \t surface \t kind \t language editions

kind is "person" for humans (instance of Q5), else "proper-noun"; language
editions is the item's sitelink count.

The inputs are QLever's (https://qlever.dev/api/wikidata) answers as TSV:
  kana.tsv:     SELECT ?item ?label ?kana ?links (SAMPLE(?class) AS ?type)
                WHERE { ?item wdt:P1814 ?kana . ?item rdfs:label ?label .
                FILTER(LANG(?label) = "ja") OPTIONAL { ?item wikibase:sitelinks ?links }
                OPTIONAL { ?item wdt:P31 ?class } } GROUP BY ?item ?label ?kana ?links
  katakana.tsv: SELECT ?item ?label ?links (SAMPLE(?class) AS ?type)
                WHERE { ?item rdfs:label ?label . FILTER(LANG(?label) = "ja")
                FILTER(REGEX(STR(?label), "^[ァ-ヺー・＝ ]+$")) ?item wikibase:sitelinks ?links .
                FILTER(?links >= 3) OPTIONAL { ?item wdt:P31 ?class } } GROUP BY ?item ?label ?links

Usage:
  python scripts/ja/build-japanese-names-words.py --kana kana.tsv --katakana katakana.tsv --out words.tsv
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

HUMAN = "<http://www.wikidata.org/entity/Q5>"
KANJI = r"㐀-䶿一-鿿豈-﫿々〆ヶ"
SURFACE = re.compile(rf"^[{KANJI}ぁ-ゖァ-ヺー・＝]{{2,24}}$")
READING = re.compile(r"^[ぁ-ゖー]{2,40}$")
# Separators names are written with but not typed.
UNTYPED = str.maketrans("", "", " 　・＝=​‌‍﻿")


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def literal(field: str) -> str:
    """A TSV literal ("text"@ja or "text") as its text."""
    match = re.match(r'^"(.*)"(?:@[\w-]+)?$', field)
    return match.group(1) if match else field


def rows(path: Path):
    with open(path, encoding="utf-8") as f:
        next(f, None)
        for line in f:
            yield line.rstrip("\n").split("\t")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kana", type=Path, required=True)
    parser.add_argument("--katakana", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    words: dict[tuple[str, str], tuple[str, int]] = {}

    def add(reading: str, surface: str, kind: str, links: int) -> None:
        if not READING.match(reading) or not SURFACE.match(surface) or surface == reading:
            return
        key = (reading, surface)
        known = words.get(key)
        if known is None or links > known[1]:
            words[key] = (kind if known is None or known[0] != "person" else "person", links)

    for fields in rows(args.kana):
        if len(fields) < 3 or "/entity/Q" not in fields[0]:
            continue  # lexemes and forms are words, not names
        label = literal(fields[1])
        reading = hiragana(literal(fields[2])).translate(UNTYPED)
        links = int(fields[3]) if len(fields) > 3 and fields[3].isdigit() else 0
        kind = "person" if len(fields) > 4 and fields[4] == HUMAN else "proper-noun"
        add(reading, label.translate(str.maketrans("", "", " 　")), kind, links)
    for fields in rows(args.katakana):
        if len(fields) < 3 or "/entity/Q" not in fields[0]:
            continue
        label = literal(fields[1]).strip()
        links = int(fields[2]) if fields[2].isdigit() else 0
        kind = "person" if len(fields) > 3 and fields[3] == HUMAN else "proper-noun"
        surface = label.replace(" ", "・").replace("＝", "・")
        add(hiragana(label).translate(UNTYPED), surface, kind, links)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for (reading, surface), (kind, links) in sorted(words.items()):
            f.write(f"{reading}\t{surface}\t{kind}\t{links}\n")
    print(len(words), "words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
