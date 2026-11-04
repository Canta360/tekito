#!/usr/bin/env python3
"""Builds japanese-compounds: how readily each kanji, read one way, ends or
starts a compound, so the converter can put together compounds no
dictionary has (皇妃 from 皇 and 妃; 燃焼筒 from 燃焼 and 筒).

Counted from the Mozc dictionary's own compounds: a word of kanji that is
another word and one more kanji, each read as the dictionary reads them
(燃焼筒, ねんしょうとう: 燃焼 ねんしょう and 筒 とう) counts once for 筒
read とう as an end, and one that is a kanji and another word (再利用) once
for 再 read さい as a start. Readings that change in a compound (本棚,
ほんだな) do not count. Each row: kanji, reading, compounds it ends,
compounds it starts.

  python scripts/ja/build-japanese-compounds.py --mozc <Mozc src/data> --out <file>
"""

from __future__ import annotations

import argparse
import collections
from pathlib import Path


def is_kanji(c: str) -> bool:
    return "一" <= c <= "鿿" or c == "々"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mozc", type=Path, required=True, help="Mozc src/data (dictionary_oss inside)")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    readings: dict[str, set[str]] = collections.defaultdict(set)
    for path in sorted((args.mozc / "dictionary_oss").glob("dictionary*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) >= 5:
                readings[fields[4]].add(fields[0])

    ends: collections.Counter[tuple[str, str]] = collections.Counter()
    starts: collections.Counter[tuple[str, str]] = collections.Counter()
    for word, word_readings in readings.items():
        if len(word) < 2 or not all(is_kanji(c) for c in word):
            continue
        for reading in word_readings:
            last, rest = word[-1], word[:-1]
            for r in readings.get(last, ()):
                if reading.endswith(r) and reading[: -len(r)] in readings.get(rest, ()):
                    ends[(last, r)] += 1
            first, rest = word[0], word[1:]
            for r in readings.get(first, ()):
                if reading.startswith(r) and reading[len(r):] in readings.get(rest, ()):
                    starts[(first, r)] += 1

    rows = sorted(set(ends) | set(starts))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as out:
        for kanji, reading in rows:
            out.write(f"{kanji}\t{reading}\t{ends[(kanji, reading)]}\t{starts[(kanji, reading)]}\n")
    print(f"{len(rows)} kanji readings, {sum(ends.values())} ends, {sum(starts.values())} starts")


if __name__ == "__main__":
    main()
