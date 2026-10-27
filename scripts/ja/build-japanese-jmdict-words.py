"""Lists words from JMdict and JMnedict (EDRDG, CC BY-SA 4.0) with their
readings, for build-japanese-packs.py --jmdict-words, a pack of their own
for their license:

- JMdict's set phrases (四字熟語, idioms, proverbs) as "phrase";
- JMnedict's place, station, company and organization names as
  "proper-noun", and family names as "person".

Written out as build-japanese-wikipedia-words.py writes:

  reading \t surface \t kind \t language editions

(editions is always 0: JMdict does not say how common a name is.)

Usage:
  python scripts/ja/build-japanese-jmdict-words.py --jmdict JMdict_e.gz --jmnedict JMnedict.xml.gz --out words.tsv
"""

from __future__ import annotations

import argparse
import gzip
import re
import xml.etree.ElementTree as ET
from pathlib import Path

KANJI = r"㐀-䶿一-鿿豈-﫿々〆ヶ"
SURFACE = re.compile(rf"^[{KANJI}ぁ-ゖァ-ヺー・]{{2,24}}$")
HAS_KANJI = re.compile(rf"[{KANJI}]")
READING = re.compile(r"^[ぁ-ゖー]{2,40}$")
# Entity texts (JMdict's DTD expands &yoji; and the others to these).
PHRASES = {"yojijukugo", "idiomatic expression", "proverb"}
NAMES = {"place name": "proper-noun", "railway station": "proper-noun", "company name": "proper-noun",
         "organization name": "proper-noun", "family or surname": "person"}
# Kanji forms not to offer: rare, irregular, outdated or for search only.
UNUSED = ("rarely", "irregular", "out-dated", "outdated", "search-only", "ateji")


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def forms(entry: ET.Element) -> list[tuple[str, str]]:
    """(reading, surface) pairs of an entry's kanji and reading forms."""
    kanji = []
    for k in entry.findall("k_ele"):
        info = " ".join(i.text or "" for i in k.findall("ke_inf"))
        if not any(word in info for word in UNUSED):
            kanji.append(k.findtext("keb") or "")
    pairs = []
    for r in entry.findall("r_ele"):
        reading = hiragana(r.findtext("reb") or "")
        if r.find("re_nokanji") is not None:
            continue
        restricted = [x.text for x in r.findall("re_restr")]
        for surface in kanji:
            if not restricted or surface in restricted:
                pairs.append((reading, surface))
    return pairs


def entries(path: Path):
    with gzip.open(path, "rb") as f:
        for _, element in ET.iterparse(f, events=("end",)):
            if element.tag == "entry":
                yield element
                element.clear()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jmdict", type=Path, required=True)
    parser.add_argument("--jmnedict", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    words: dict[tuple[str, str], str] = {}

    def add(reading: str, surface: str, kind: str) -> None:
        if READING.match(reading) and SURFACE.match(surface) and HAS_KANJI.search(surface):
            words.setdefault((reading, surface), kind)

    for entry in entries(args.jmdict):
        misc = {m.text for sense in entry.findall("sense") for m in sense.findall("misc")}
        if misc & PHRASES:
            for reading, surface in forms(entry):
                add(reading, surface, "phrase")
    for entry in entries(args.jmnedict):
        types = [t.text for t in entry.iter("name_type")]
        kind = next((NAMES[t] for t in types if t in NAMES), None)
        if kind:
            for reading, surface in forms(entry):
                add(reading, surface, kind)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        for (reading, surface), kind in sorted(words.items()):
            f.write(f"{reading}\t{surface}\t{kind}\t0\n")
    counts = {}
    for kind in words.values():
        counts[kind] = counts.get(kind, 0) + 1
    print(len(words), "words", counts)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
