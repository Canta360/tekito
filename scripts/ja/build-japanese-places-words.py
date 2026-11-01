"""Lists Japan's place names with their readings from Japan Post's postal
code data (utf_ken_all.csv; Japan Post claims no copyright on it), for
build-japanese-packs.py --places-words: prefectures, cities, towns and
villages, and the town areas under them (長居, 繋, 若葉).
Written out as build-japanese-wikipedia-words.py writes:

  reading \t surface \t proper-noun \t 0

Left out: the notes the data uses in place of an area (以下に掲載がない場合
and the like) and areas with a part in brackets.

Usage:
  python scripts/ja/build-japanese-places-words.py --ken-all utf_ken_all.csv --out words.tsv
"""

from __future__ import annotations

import argparse
import csv
import re
import unicodedata
from pathlib import Path

NOTES = ("以下に掲載がない場合", "の次に番地がくる場合", "一円")
LONGEST_READING = 20
# Columns: the readings (half-width katakana) and names of the prefecture,
# the city and the town area.
PLACES = ((3, 6), (4, 7), (5, 8))


def hiragana(katakana: str) -> str:
    full = unicodedata.normalize("NFKC", katakana)
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in full)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--ken-all", type=Path, required=True, help="Japan Post's utf_ken_all.csv")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    places: dict[tuple[str, str], None] = {}
    with open(args.ken_all, encoding="utf-8", newline="") as f:
        for row in csv.reader(f):
            for kana, name in ((row[r], row[n]) for r, n in PLACES):
                if not name or any(note in name for note in NOTES) or "（" in name or "(" in kana:
                    continue
                reading = hiragana(kana)
                if re.fullmatch(r"[ぁ-ゔー]+", reading) and len(reading) <= LONGEST_READING:
                    places[(reading, name)] = None
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("".join(f"{reading}\t{name}\tproper-noun\t0\n" for reading, name in places),
                        encoding="utf-8", newline="\n")
    print(f"{len(places)} places")


if __name__ == "__main__":
    main()
