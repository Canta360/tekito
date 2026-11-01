#!/usr/bin/env python3
"""Builds japanese-letter-odds: how much more an English spelling than the
romaji of a Japanese reading each letter is, after the two before it, so
Mix can tell an English word no list has (cappuccino, gibberellin) from
Japanese typed in romaji (kyouha).

The English side is the words of data/en/frequency; the Japanese side the
readings of the Mozc dictionary, typed as romaji the way
eval/ja/kana-to-keys.py types them and in Kunrei spellings too (si, tu). Each row: three letters (^ before a
word, $ after it), and ln p(last | two before) in English less in romaji,
with add-0.1 smoothing over the 28 symbols; every one of the 28^3 triples
is listed.

  python scripts/ja/build-japanese-letter-odds.py --mozc <Mozc src/data> --out data/ja/japanese-letter-odds
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.util
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYMBOLS = "^abcdefghijklmnopqrstuvwxyz$"
SMOOTHING = 0.1
# Kunrei spellings of the Hepburn ones kana-to-keys.py types.
KUNREI = [("sha", "sya"), ("shu", "syu"), ("sho", "syo"), ("shi", "si"), ("cha", "tya"), ("chu", "tyu"),
          ("cho", "tyo"), ("chi", "ti"), ("tsu", "tu"), ("fu", "hu"), ("ja", "zya"), ("ju", "zyu"), ("jo", "zyo"),
          ("ji", "zi")]


def load_to_keys():
    path = ROOT / "eval" / "ja" / "kana-to-keys.py"
    spec = importlib.util.spec_from_file_location("kana_to_keys", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.to_keys


def counts(words):
    triples, pairs = collections.Counter(), collections.Counter()
    for word in words:
        w = "^^" + word + "$"
        for i in range(2, len(w)):
            triples[w[i - 2:i + 1]] += 1
            pairs[w[i - 2:i]] += 1
    return triples, pairs


def log_p(model, triple):
    triples, pairs = model
    return math.log((triples[triple] + SMOOTHING) / (pairs[triple[:2]] + SMOOTHING * len(SYMBOLS)))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--mozc", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "ja" / "japanese-letter-odds")
    args = parser.parse_args()
    english = [line.split("\t", 1)[0] for line in
               (ROOT / "data" / "en" / "frequency" / "word-scores.tsv").read_text(encoding="utf-8").splitlines()]
    english = [w for w in english if re.fullmatch(r"[a-z]{3,}", w)]
    to_keys = load_to_keys()
    readings = set()
    for path in sorted((args.mozc / "dictionary_oss").glob("dictionary0*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            reading = line.split("\t", 1)[0]
            if 2 <= len(reading) <= 10:
                readings.add(reading)
    romaji = []
    for keys in (to_keys(r) for r in sorted(readings)):
        if not keys or not re.fullmatch(r"[a-z]+", keys):
            continue
        # As typed both ways: Hepburn (shi, tsu) and Kunrei (si, tu).
        romaji.append(keys)
        kunrei = keys
        for hepburn, other in KUNREI:
            kunrei = kunrei.replace(hepburn, other)
        if kunrei != keys:
            romaji.append(kunrei)
    e, j = counts(english), counts(romaji)
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for a in SYMBOLS:
        for b in SYMBOLS:
            for c in SYMBOLS:
                triple = a + b + c
                rows.append(f"{triple}\t{log_p(e, triple) - log_p(j, triple):.3f}")
    data = args.out / "odds.tsv"
    data.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    (args.out / "NOTICE").write_text(
        "How English spellings and the romaji of Japanese readings differ, letter by letter: counts\n"
        "derived from TEKITO's English word list (data/en/frequency) and the readings of the Mozc\n"
        "dictionary (IPAdic and BSD 3-Clause, see the japanese-core pack's NOTICE). No word or text\n"
        "is included, only these numbers. Built by scripts/ja/build-japanese-letter-odds.py.\n"
        "\n"
        "The English word list is derived from the Leipzig Corpora Collection English News 2025\n"
        "1M sentence download, under CC BY 4.0.\n"
        "Attribution: (c) 2026 Universitat Leipzig / Sachsische Akademie der Wissenschaften / InfAI.\n"
        "Source: https://wortschatz.uni-leipzig.de/en/download/eng\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-letter-odds", "display_name": "English or Romaji Letter Odds", "schema_version": 1,
        "version": "2026.10", "language": "ja-JP", "type": "letter-odds", "format": "tsv-v1", "file": "odds.tsv",
        "entry_count": len(rows), "sha256": {"file": sha256(data)}, "license": "IPAdic + BSD-3-Clause (Mozc), CC-BY-4.0 (Leipzig), TEKITO-OWNED",
        "source": "scripts/ja/build-japanese-letter-odds.py", "notice_file": "NOTICE",
        "english_words": len(english), "japanese_readings": len(romaji),
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"japanese-letter-odds: {len(rows)} triples from {len(english)} English words, {len(romaji)} readings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
