#!/usr/bin/env python3
"""Word vectors for the topic of a sentence, to choose among words that
read alike (戴冠 among 体感, 耐寒): words used together get vectors that
point the same way, so the words around a conversion show which one fits.

The words are what the sentence check already takes as content words:
runs of kanji, and katakana words. Trained (word2vec skip-gram, gensim) on
the zenz-v2.5 dataset's llm-jp-corpus-v3 part (ODC-BY 1.0); a share of its
sentences is kept apart to measure on.

  corpus   writes train.txt (one sentence's words per line) and
           test.jsonl (the kept-apart sentences)
  train    the vectors (words.kv)
  test     how often the word meant is chosen among those that read alike,
           by the dictionary's order alone and with the topic

  train-japanese-topics.py corpus --out DIR --zenz FILE.jsonl [--every N] [--exclude-dir D] [--exclude-sha1 F]
  train-japanese-topics.py train  --out DIR [--dim 64]
  train-japanese-topics.py test   --out DIR --mozc <Mozc dictionary_oss>
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

WORD = re.compile(r"[一-鿿々]+|[ァ-ヺー]{2,}")
TEST_EVERY = 50  # one sentence in this many is kept apart


def words(text: str) -> list[str]:
    return WORD.findall(text)


def excluded_test(args):
    path = Path(__file__).resolve().parent / "train-japanese-wordlm.py"
    spec = importlib.util.spec_from_file_location("wordlm", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.excluded_test(args)


def corpus(args) -> None:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    kept = tested = 0
    with open(out / "train.txt", "w", encoding="utf-8", newline="\n") as train, \
            open(out / "test.jsonl", "w", encoding="utf-8", newline="\n") as test:
        for number, line in enumerate(open(args.zenz, encoding="utf-8")):
            if number % args.every:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            text = (row.get("output") or "").strip()
            context = (row.get("left_context") or "").strip()
            if not text or left_out(text):
                continue
            if (number // args.every) % TEST_EVERY == 0:
                test.write(json.dumps({"context": context, "text": text}, ensure_ascii=False) + "\n")
                tested += 1
                continue
            found = words(context) + words(text)
            if len(found) > 1:
                train.write(" ".join(found) + "\n")
                kept += 1
    print(f"{kept} sentences to train on, {tested} kept apart")


def train(args) -> None:
    from gensim.models import Word2Vec
    from gensim.models.word2vec import LineSentence

    out = Path(args.out)
    model = Word2Vec(LineSentence(str(out / "train.txt")), vector_size=args.dim, window=args.window,
                     min_count=args.min_count, sg=1, negative=5, sample=1e-4, workers=args.workers,
                     epochs=args.epochs)
    model.wv.save(str(out / "words.kv"))
    print(f"{len(model.wv)} words")


def homophones(mozc: Path) -> dict[str, list[tuple[str, int]]]:
    """Kanji words by reading, each with its lowest cost, for readings that
    have more than one."""
    best: dict[tuple[str, str], int] = {}
    for path in sorted(mozc.glob("dictionary0*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split("\t")
            if len(parts) < 5:
                continue
            reading, cost, surface = parts[0], int(parts[3]), parts[4]
            if len(surface) < 2 or not re.fullmatch(r"[一-鿿々]+", surface):
                continue
            key = (reading, surface)
            best[key] = min(best.get(key, cost), cost)
    groups = defaultdict(list)
    for (reading, surface), cost in best.items():
        groups[reading].append((surface, cost))
    return {r: sorted(g, key=lambda x: x[1]) for r, g in groups.items() if len(g) > 1}


def test(args) -> None:
    import numpy as np
    from gensim.models import KeyedVectors

    out = Path(args.out)
    wv = KeyedVectors.load(str(out / "words.kv"))
    groups = homophones(Path(args.mozc))
    readings_of = defaultdict(list)
    for reading, group in groups.items():
        for surface, _ in group:
            readings_of[surface].append(reading)
    items = []
    for line in open(out / "test.jsonl", encoding="utf-8"):
        row = json.loads(line)
        found = words(row["text"])
        around = words(row["context"]) + found
        for i, word in enumerate(found):
            for reading in readings_of.get(word, []):
                group = [s for s, _ in groups[reading] if s in wv]
                if word in wv and len(group) > 1:
                    others = [w for w in around if w != word and w in wv]
                    items.append((word, group, others))
    print(f"{len(items)} words that read like another")
    hard = [it for it in items if it[1][0] != it[0]]

    def choose(item, weight):
        word, group, others = item
        if not others or weight == 0:
            return group[0]
        topic = np.mean([wv[w] / np.linalg.norm(wv[w]) for w in others], axis=0)
        topic /= np.linalg.norm(topic) or 1
        # The dictionary's order as a prior: first best, then less.
        score = lambda k, s: -k * 0.5 + weight * float(np.dot(wv[s] / np.linalg.norm(wv[s]), topic))
        return max(enumerate(group), key=lambda ks: score(*ks))[1]

    for weight in (0, 1, 2, 4, 8, 16):
        right = sum(choose(it, weight) == it[0] for it in items)
        right_hard = sum(choose(it, weight) == it[0] for it in hard)
        kept = sum(choose(it, weight) == it[0] for it in items if it[1][0] == it[0])
        print(f"weight {weight:>2}: all {100 * right / len(items):5.1f}%  "
              f"not the dictionary's first {100 * right_hard / max(len(hard), 1):5.1f}% ({len(hard)})  "
              f"dictionary's first kept {100 * kept / max(len(items) - len(hard), 1):5.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("step", choices=["corpus", "train", "test"])
    parser.add_argument("--out", required=True)
    parser.add_argument("--zenz")
    parser.add_argument("--every", type=int, default=1)
    parser.add_argument("--exclude-dir", action="append", default=[])
    parser.add_argument("--exclude-sha1", action="append", default=[])
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--window", type=int, default=10)
    parser.add_argument("--min-count", type=int, default=20)
    parser.add_argument("--epochs", type=int, default=3)
    parser.add_argument("--workers", type=int, default=16)
    parser.add_argument("--mozc")
    args = parser.parse_args()
    {"corpus": corpus, "train": train, "test": test}[args.step](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
