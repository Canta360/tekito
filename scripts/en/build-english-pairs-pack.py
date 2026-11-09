"""Builds the english-pairs data pack: how much English words go together
when they stand near each other in a sentence (fix ... error, cure ...
disease), as base forms, for choosing among Japanese words by what they mean
in English (直す: fix, correct; 治す: cure, heal) where the Japanese data has
no tie between the words.

Counted in the Leipzig Corpora Collection's English news 2025 corpus (1M
sentences, CC BY 4.0): each content word with the next few content words
after it (function words left out), so a verb and its object meet with an
article between them. A pair is kept when it was seen often enough and
clearly more often than chance; its score is the pointwise mutual
information in nats.

The rows are "first second<TAB>pmi" with the two words in byte order, so
that one lookup finds a pair either way round.

Usage:
  python scripts/en/build-english-pairs-pack.py --sentences <eng_news_2025_1M-sentences.txt> \\
      --lexicon <standard-english/lexicon.txt> --scores <frequency/word-scores.tsv>
"""

from __future__ import annotations

import argparse
import importlib.util
import math
import re
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
WORD = re.compile(r"[a-z]+(?:'[a-z]+)?")
# Content words this many after a word are its neighbours.
WINDOW = 3
# A pair seen fewer times, or not this much likelier than chance, is left out.
MIN_PAIR = 3
MIN_PMI = 1.0
# Words seen fewer times are too rare to count.
MIN_WORD = 5
FUNCTION_WORDS = set("""
a an the this that these those my your his her its our their some any no every each all both either neither
i me you he him she it we us they them myself yourself himself herself itself ourselves themselves
and or but nor so yet if then than because as while when where whether though although
of in on at to for from by with about into onto over under after before between through during without within
up down out off away back again also too very just only even still already not never
is am are was were be been being have has had having do does did doing done
will would shall should can could may might must
there here what which who whom whose how why
s t d ll m re ve don doesn didn isn aren wasn weren won wouldn couldn shouldn
""".split())


def lemma(word: str, known: dict[str, float]) -> str:
    """The base form of `word` (errors: error, fixed: fix, studies: study),
    the likeliest English word the usual endings give, else the word."""
    if word.endswith("'s"):
        word = word[:-2]
    candidates = [word]
    if word.endswith("ies") and len(word) > 4:
        candidates.append(word[:-3] + "y")
    if word.endswith("es") and len(word) > 3:
        candidates.append(word[:-2])
    if word.endswith("s") and not word.endswith("ss") and len(word) > 3:
        candidates.append(word[:-1])
    for ending in ("ing", "ed"):
        if word.endswith(ending) and len(word) > len(ending) + 2:
            stem = word[: -len(ending)]
            candidates += [stem, stem + "e"]
            if len(stem) > 2 and stem[-1] == stem[-2]:
                candidates.append(stem[:-1])
            if ending == "ed" and stem.endswith("i"):
                candidates.append(stem[:-1] + "y")
    bases = [c for c in candidates[1:] if c in known]
    if not bases:
        return word
    # The word itself when it is commoner than any base (news, during).
    best = max(bases, key=lambda c: known[c])
    return word if word in known and known[word] > known[best] + 0.5 else best


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sentences", type=Path, required=True)
    parser.add_argument("--lexicon", type=Path, required=True)
    parser.add_argument("--scores", type=Path, required=True)
    args = parser.parse_args()

    spec = importlib.util.spec_from_file_location("full", ROOT / "scripts" / "en" / "build-full-data-packs.py")
    full = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(full)

    lexicon = {line.split("\t")[0].strip().lower() for line in open(args.lexicon, encoding="utf-8")
               if line.strip() and not line.startswith("#")}
    known: dict[str, float] = {}
    for line in open(args.scores, encoding="utf-8"):
        fields = line.rstrip("\n").split("\t")
        if len(fields) >= 2 and fields[0] in lexicon:
            known[fields[0]] = float(fields[1])

    ids: dict[str, int] = {}
    cache: dict[str, int] = {}
    counts: list[int] = []
    pairs: list[np.ndarray] = []
    buffer: list[int] = []
    total = 0
    for line in open(args.sentences, encoding="utf-8"):
        text = line.split("\t", 1)[-1].lower()
        sentence = []
        for word in WORD.findall(text):
            if word in FUNCTION_WORDS:
                continue
            at = cache.get(word)
            if at is None:
                base = lemma(word, known)
                if base in FUNCTION_WORDS or base not in known:
                    at = -1
                else:
                    at = ids.setdefault(base, len(ids))
                    if at == len(counts):
                        counts.append(0)
                cache[word] = at
            if at >= 0:
                sentence.append(at)
        for i, first in enumerate(sentence):
            counts[first] += 1
            total += 1
            for second in sentence[i + 1 : i + 1 + WINDOW]:
                if second != first:
                    low, high = (first, second) if first < second else (second, first)
                    buffer.append(low * 2_000_000 + high)
        if len(buffer) > 5_000_000:
            pairs.append(np.array(buffer, dtype=np.int64))
            buffer = []
    pairs.append(np.array(buffer, dtype=np.int64))
    keys, seen = np.unique(np.concatenate(pairs), return_counts=True)
    words = [""] * len(ids)
    for word, at in ids.items():
        words[at] = word
    count = np.array(counts, dtype=np.float64)
    pair_total = float(seen.sum())
    low = keys // 2_000_000
    high = keys % 2_000_000
    keep = (seen >= MIN_PAIR) & (count[low] >= MIN_WORD) & (count[high] >= MIN_WORD)
    pmi = np.log(seen * (total * total / pair_total) / (count[low] * count[high]))
    keep &= pmi >= MIN_PMI
    rows = []
    for a, b, score in zip(low[keep], high[keep], pmi[keep]):
        first, second = sorted((words[a], words[b]), key=lambda w: w.encode("utf-8"))
        rows.append((f"{first} {second}", score))
    rows.sort(key=lambda row: row[0].encode("utf-8"))

    pack = full.reset_pack("english-pairs")
    with (pack / "pairs.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for key, score in rows:
            output.write(f"{key}\t{score:.2f}\n")
    manifest = full.finish_pack(
        "english-pairs", "English Word Pairs", "leipzig-news-2025", "word-pairs", "pairs.tsv",
        "TEKITO_PAIRS_INDEX_V1", 1, "CC-BY-4.0",
        "https://wortschatz.uni-leipzig.de/en/download/English (eng_news_2025_1M)",
        "How often English words stand near each other, counted by TEKITO in the Leipzig Corpora "
        "Collection's English news 2025 corpus (1M sentences), under CC BY 4.0 "
        "(https://creativecommons.org/licenses/by/4.0/): D. Goldhahn, T. Eckart and U. Quasthoff, "
        "\"Building Large Monolingual Dictionaries at the Leipzig Corpora Collection\", LREC 2012. "
        "The pack holds word pairs and numbers only; it contains no sentences from the corpus.")
    print(f"Built {manifest['display_name']}: {manifest['entry_count']:,} pairs of {len(ids):,} words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
