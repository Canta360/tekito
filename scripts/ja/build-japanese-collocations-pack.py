"""Builds the japanese-collocations data pack: how often a word stands
before a particle and another word after it (エラー を 直, 住所 を 併記,
少年誌 に 連載), so that conversion can tell a likely pair from one that
never meets though both words are common (エラー を 治: never).

Words are runs of kanji or katakana (the content of a phrase): the run
right before one of を に が で へ と の, and the run right after it, which for
a verb is its stem (直, of 直す, 直した), so homophones that differ in their
kanji (直す, 治す) are told apart without knowing their forms; or, where kana
follows the particle, its first two kana (見当 を つけ).

Counted in the Leipzig Corpora Collection's Japanese news 2019 and Wikipedia
2021 corpora (CC BY 4.0) and in the first lines of the zenz-v2.5 dataset's
llm-jp-corpus-v3 part (ODC-BY 1.0), less any line an evaluation set has.

Rows, sorted bytewise by their first field:
  "X p Y<TAB>count"  a pair seen often enough
  "X p<TAB>count"    all pairs with X before p
  "~ Y<TAB>count"    all pairs with Y after a particle
  "~~<TAB>count"     all pairs

Usage:
  python scripts/ja/build-japanese-collocations-pack.py --leipzig <leipzig-ja folder> \\
      --zenz <train_llm-jp-corpus-v3.jsonl> --zenz-lines 10000000 --exclude '<eval keys files glob>'
"""

from __future__ import annotations

import argparse
import collections
import glob
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from datapacks import pack_dir  # noqa: E402

CONTENT = "[㐀-鿿々ァ-ヺー]"
# After the particle: a run of kanji or katakana, or else the first two kana
# of what follows (見当 を つけ: a verb written in kana).
PAIR = re.compile(f"(?<!{CONTENT})({CONTENT}{{1,8}})([をにがでへとの])({CONTENT}{{1,8}}|[ぁ-ゖ]{{2}})")
LEIPZIG = ["jpn_newscrawl_2019_1M", "jpn_wikipedia_2021_1M"]
MIN_PAIR = 3
MIN_SIDE = 5


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--leipzig", type=Path, required=True)
    parser.add_argument("--zenz", type=Path)
    parser.add_argument("--zenz-lines", type=int, default=0)
    parser.add_argument("--exclude", action="append", default=[])
    args = parser.parse_args()

    kept_apart: set[str] = set()
    for pattern in args.exclude:
        for path in glob.glob(pattern):
            for line in open(path, encoding="utf-8"):
                fields = line.rstrip("\n").split("\t")
                kept_apart.update(f for f in fields[3:] if f)

    pairs: collections.Counter[str] = collections.Counter()

    def count(text: str) -> None:
        # Overlapping pairs: 住所を併記 and 併記を... both count.
        at = 0
        while True:
            match = PAIR.search(text, at)
            if not match:
                return
            pairs[f"{match.group(1)} {match.group(2)} {match.group(3)}"] += 1
            at = match.start(3)

    for corpus in LEIPZIG:
        for line in open(args.leipzig / corpus / f"{corpus}-sentences.txt", encoding="utf-8"):
            count(line.split("\t", 1)[-1])
    skipped = 0
    if args.zenz and args.zenz_lines:
        with open(args.zenz, encoding="utf-8") as source:
            for n, line in enumerate(source):
                if n >= args.zenz_lines:
                    break
                text = json.loads(line).get("output") or ""
                if text in kept_apart:
                    skipped += 1
                    continue
                count(text)

    left: collections.Counter[str] = collections.Counter()
    right: collections.Counter[str] = collections.Counter()
    total = 0
    for key, seen in pairs.items():
        x, p, y = key.split(" ")
        left[f"{x} {p}"] += seen
        right[y] += seen
        total += seen
    rows = [(key, seen) for key, seen in pairs.items() if seen >= MIN_PAIR]
    rows += [(key, seen) for key, seen in left.items() if seen >= MIN_SIDE]
    rows += [(f"~ {y}", seen) for y, seen in right.items() if seen >= MIN_SIDE]
    rows.append(("~~", total))
    rows.sort(key=lambda row: row[0].encode("utf-8"))

    pack = pack_dir("japanese-collocations")
    pack.mkdir(parents=True, exist_ok=True)
    data = pack / "pairs.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as output:
        for key, seen in rows:
            output.write(f"{key}\t{seen}\n")
    (pack / "NOTICE").write_text(
        "How often Japanese words stand on either side of a particle, counted by TEKITO\n"
        "(scripts/ja/build-japanese-collocations-pack.py) in the Leipzig Corpora Collection's Japanese news 2019 and\n"
        "Wikipedia 2021 corpora, under CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/): D. Goldhahn,\n"
        "T. Eckart and U. Quasthoff, \"Building Large Monolingual Dictionaries at the Leipzig Corpora Collection\",\n"
        "LREC 2012; and in text from the zenz-v2.5-dataset (https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset),\n"
        "its llm-jp-corpus-v3 part, under the Open Data Commons Attribution License (ODC-BY 1.0):\n"
        "https://opendatacommons.org/licenses/by/1-0/\n"
        "The pack holds word pairs and counts only; it contains no sentences from the corpora.\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-collocations",
        "display_name": "Japanese Word Pairs",
        "schema_version": 1,
        "version": "2026.11",
        "language": "ja-JP",
        "type": "japanese-collocations",
        "file": "pairs.tsv",
        "entry_count": len(rows),
        "sha256": {"file": hashlib.sha256(data.read_bytes()).hexdigest().upper()},
        "license": "TEKITO-OWNED",
        "source": "Leipzig Corpora Collection jpn_newscrawl_2019_1M and jpn_wikipedia_2021_1M (CC BY 4.0); "
                  "zenz-v2.5-dataset llm-jp-corpus-v3 part (ODC-BY 1.0)",
        "notice_file": "NOTICE",
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"Built {manifest['display_name']}: {len(pairs):,} pairs seen, {len(rows):,} rows, "
          f"{total:,} in all, {skipped:,} evaluation lines left out")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
