"""Builds the japanese-homophones data pack: for words that share their
reading with another word (信任 and 新任, both しんにん), the words their
sentences most often have besides (信任: 内閣, 議会, 投票; 新任: 教師, 着任),
so that conversion can tell homophones apart by what else the sentence
says, word by word rather than by a general topic.

The words are those of Mozc's dictionary written with kanji, two to four
characters long, whose reading has another such word. In the text, a word
is found inside the runs of kanji and katakana it is part of (新任 in
新任教師), and the other runs of two characters or more in the sentence are
its context. A context word is kept when it is in thirty sentences or more,
was seen with the word at least three times and clearly more often than
chance (pointwise mutual information); for each word, the few that tell
most (seen often, and often only with it).

Counted in the Leipzig Corpora Collection's Japanese news 2019 and Wikipedia
2021 corpora (CC BY 4.0) and in the first lines of the zenz-v2.5 dataset's
llm-jp-corpus-v3 part (ODC-BY 1.0), less any line an evaluation set has.

Rows "word context<TAB>pmi", sorted bytewise.

Usage:
  python scripts/ja/build-japanese-homophones-pack.py --mozc <dictionary_oss> --leipzig <leipzig-ja folder> \\
      --zenz <train_llm-jp-corpus-v3.jsonl> --zenz-lines 10000000 --exclude '<eval keys files glob>'
"""

from __future__ import annotations

import argparse
import collections
import glob
import hashlib
import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from datapacks import pack_dir  # noqa: E402

RUN = re.compile("[㐀-鿿々ァ-ヺー]{2,}")
KANJI = re.compile("[㐀-鿿々]")
LEIPZIG = ["jpn_newscrawl_2019_1M", "jpn_wikipedia_2021_1M"]
MIN_PAIR = 3
MIN_PMI = 1.0
# A context word seen in fewer sentences is too rare to tell much.
MIN_CONTEXT = 30
PER_WORD = 30
# Passes over the text, each counting a share of the words, so the counts fit in memory.
SHARDS = 4


def homophones(mozc: Path) -> set[str]:
    by_reading: dict[str, set[str]] = collections.defaultdict(set)
    for path in sorted(mozc.glob("dictionary0*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) >= 5 and 2 <= len(fields[4]) <= 4 and KANJI.search(fields[4]) and RUN.fullmatch(fields[4]):
                by_reading[fields[0]].add(fields[4])
    words: set[str] = set()
    for surfaces in by_reading.values():
        if len(surfaces) >= 2:
            words |= surfaces
    return words


def sentences(args, kept_apart: set[str]):
    for corpus in LEIPZIG:
        for line in open(args.leipzig / corpus / f"{corpus}-sentences.txt", encoding="utf-8"):
            yield line.split("\t", 1)[-1]
    if args.zenz and args.zenz_lines:
        with open(args.zenz, encoding="utf-8") as source:
            for n, line in enumerate(source):
                if n >= args.zenz_lines:
                    break
                text = json.loads(line).get("output") or ""
                if text not in kept_apart:
                    yield text


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mozc", type=Path, required=True)
    parser.add_argument("--leipzig", type=Path, required=True)
    parser.add_argument("--zenz", type=Path)
    parser.add_argument("--zenz-lines", type=int, default=0)
    parser.add_argument("--exclude", action="append", default=[])
    args = parser.parse_args()

    kept_apart: set[str] = set()
    for pattern in args.exclude:
        for path in glob.glob(pattern):
            for line in open(path, encoding="utf-8"):
                kept_apart.update(f for f in line.rstrip("\n").split("\t")[3:] if f)
    targets = homophones(args.mozc)
    print(f"{len(targets):,} words that share a reading", flush=True)

    # Each sentence once: its runs, as found.
    context_count: collections.Counter[str] = collections.Counter()
    word_count: collections.Counter[str] = collections.Counter()
    total = 0
    rows: list[tuple[str, float]] = []
    for shard in range(SHARDS):
        pairs: collections.Counter[tuple[str, str]] = collections.Counter()
        for text in sentences(args, kept_apart):
            runs = set(RUN.findall(text))
            if len(runs) < 2:
                continue
            found: set[tuple[str, str]] = set()
            for run in runs:
                for length in range(2, min(4, len(run)) + 1):
                    for at in range(len(run) - length + 1):
                        word = run[at:at + length]
                        if word in targets and hash(word) % SHARDS == shard:
                            found.add((word, run))
            if shard == 0:
                total += 1
                context_count.update(runs)
            for word, own in found:
                for context in runs:
                    if context != own and word not in context:
                        pairs[(word, context)] += 1
            for word in {w for w, _ in found}:
                word_count[word] += 1
        for (word, context), seen in pairs.items():
            if seen < MIN_PAIR or context_count[context] < MIN_CONTEXT:
                continue
            pmi = math.log(seen * total / (word_count[word] * context_count[context]))
            if pmi >= MIN_PMI:
                rows.append((f"{word} {context}", pmi, seen))
        print(f"pass {shard + 1}/{SHARDS}: {len(rows):,} rows so far", flush=True)

    # The strongest few for each word.
    by_word: dict[str, list[tuple[str, float]]] = collections.defaultdict(list)
    for key, pmi, seen in rows:
        by_word[key.split(" ", 1)[0]].append((key, pmi, seen))
    kept = []
    for word, items in by_word.items():
        # By how much they tell together: seen often, and often only with it.
        items.sort(key=lambda item: -item[1] * item[2])
        kept += [(key, pmi) for key, pmi, _ in items[:PER_WORD]]
    kept.sort(key=lambda row: row[0].encode("utf-8"))

    pack = pack_dir("japanese-homophones")
    pack.mkdir(parents=True, exist_ok=True)
    data = pack / "contexts.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as output:
        for key, pmi in kept:
            output.write(f"{key}\t{pmi:.2f}\n")
    (pack / "NOTICE").write_text(
        "Which words the sentences of Japanese homophones have besides, counted by TEKITO\n"
        "(scripts/ja/build-japanese-homophones-pack.py) for the words of Mozc's dictionary (BSD-3-Clause) in the\n"
        "Leipzig Corpora Collection's Japanese news 2019 and Wikipedia 2021 corpora, under CC BY 4.0\n"
        "(https://creativecommons.org/licenses/by/4.0/): D. Goldhahn, T. Eckart and U. Quasthoff, \"Building Large\n"
        "Monolingual Dictionaries at the Leipzig Corpora Collection\", LREC 2012; and in text from the\n"
        "zenz-v2.5-dataset (https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset), its llm-jp-corpus-v3\n"
        "part, under the Open Data Commons Attribution License (ODC-BY 1.0): https://opendatacommons.org/licenses/by/1-0/\n"
        "The pack holds word pairs and numbers only; it contains no sentences from the corpora.\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-homophones",
        "display_name": "Japanese Homophone Contexts",
        "schema_version": 1,
        "version": "2026.11",
        "language": "ja-JP",
        "type": "japanese-homophones",
        "file": "contexts.tsv",
        "entry_count": len(kept),
        "sha256": {"file": hashlib.sha256(data.read_bytes()).hexdigest().upper()},
        "license": "TEKITO-OWNED",
        "source": "Leipzig Corpora Collection jpn_newscrawl_2019_1M and jpn_wikipedia_2021_1M (CC BY 4.0); "
                  "zenz-v2.5-dataset llm-jp-corpus-v3 part (ODC-BY 1.0); words from Mozc",
        "notice_file": "NOTICE",
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"Built {manifest['display_name']}: {len(kept):,} rows for {len(by_word):,} words, {total:,} sentences")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
