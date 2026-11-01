"""Lists the nouns of SudachiDict (Apache-2.0) with their readings, for
build-japanese-packs.py --sudachi-words: common and proper nouns written
with kanji or katakana (新語, 複合語, names of people, places and things).
Written out as build-japanese-wikipedia-words.py writes:

  reading \t surface \t kind \t language editions

kind is "person" for Sudachi's 人名, "proper-noun" for its other 固有名詞,
else "noun"; language editions stands in for how common the word is, from
Sudachi's cost (TEKITO adds a word cheaper the more editions it has).

Left out: numbers, words with letters, digits or marks, and katakana that
spells out letters (ピーエイチピー for PHP: Sudachi's normalized form has
the letters).

The inputs are SudachiDict's source lexicons (small_lex.csv, core_lex.csv
and notcore_lex.csv, V1), from
http://sudachi.s3-website-ap-northeast-1.amazonaws.com/sudachidict-raw/v1/

Usage:
  python scripts/ja/build-japanese-sudachi-words.py --lex small_lex.csv core_lex.csv notcore_lex.csv --out words.tsv
"""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

KATAKANA = re.compile(r"^[ァ-ヴー]+$")
WRITTEN = re.compile(r"[一-鿿々〆ヶァ-ヺ]")
NOT_WRITTEN = re.compile(r"[A-Za-z0-9Ａ-Ｚａ-ｚ０-９!-/:-@\[-`{-~、。「」『』（）【】＃＆＊＠\s]")
LETTERS = re.compile(r"[A-Za-zＡ-Ｚａ-ｚ]")
LONGEST_READING = 24
LONGEST_SURFACE = 16

# Columns of the V1 lexicon.
HEADWORD, INDEX, COST, POS1, POS2, POS3, READING, NORMALIZED = 4, 0, 3, 5, 6, 7, 11, 12


def hiragana(katakana: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in katakana)


def editions(cost: int) -> int:
    # Sudachi's cost: lower for words used more (-1 or 0 when not set).
    if cost <= 0:
        return 3
    if cost < 5000:
        return 15
    if cost < 8000:
        return 7
    if cost < 12000:
        return 3
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lex", type=Path, nargs="+", required=True, help="SudachiDict source lexicons (V1 CSV)")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    seen: set[tuple[str, str]] = set()
    lines = []
    for path in args.lex:
        with open(path, encoding="utf-8", newline="") as f:
            rows = csv.reader(f)
            next(rows)
            for row in rows:
                if len(row) <= NORMALIZED or row[POS1] != "名詞" or row[POS2] == "数詞":
                    continue
                surface, reading = row[HEADWORD] or row[INDEX], row[READING]
                if (not KATAKANA.match(reading) or not WRITTEN.search(surface) or NOT_WRITTEN.search(surface)
                        or len(reading) > LONGEST_READING or len(surface) > LONGEST_SURFACE):
                    continue
                if LETTERS.search(row[NORMALIZED].split(",", 1)[0]):
                    continue
                kind = ("person" if row[POS2] == "固有名詞" and row[POS3] == "人名"
                        else "proper-noun" if row[POS2] == "固有名詞" else "noun")
                key = (hiragana(reading), surface)
                if key in seen:
                    continue
                seen.add(key)
                lines.append(f"{key[0]}\t{surface}\t{kind}\t{editions(int(row[COST]))}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(lines)} words")


if __name__ == "__main__":
    main()
