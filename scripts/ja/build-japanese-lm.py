#!/usr/bin/env python3
"""Builds the japanese-lm pack: which words go together in Japanese text,
for kana-kanji conversion to prefer them ("よろしく" + "お願い").

The Leipzig Corpora Collection's Japanese corpora come tokenized (IPADIC
style, like the Mozc dictionary), with every word's place in every
sentence. The sentences are put back together as word sequences and every
pair of neighbors is counted. For a pair (left, right) conversion wants
how much likelier it is than its words apart, the pointwise mutual
information PMI = ln(count(left, right) * N / (count(left) * count(right)));
a pair never seen gets ln(0.5 * N / (count(left) * count(right))), which is
negative for words common enough that they would have met.

It also keeps which content words (with kanji or katakana) occur in the
same sentences more than by chance (the corpora's sentence co-occurrence
lists, with their log-likelihood significance), so a word can be told
from its homophones by the other words of the sentence (麻酔 with 注射,
not 駐車).

lm.bin (ja-lm-v3), little-endian, read in place:
  header   see HEADER; tokens is N
  pairs    u32[slots] fingerprints (0 = empty), then u8[slots] scores:
           open addressing, linear probing, slot = hash & (slots - 1);
           score = round((PMI + 5) * 10), 1..255
  words    the same layout for single words; score = round(ln(count) * 10)
  topics   the same layout for words of one sentence, the smaller (by code
           units) first; score = round(ln(1 + significance) * 20), 1..255
The hash is FNV-1a (64 bit) over the UTF-16LE code units of the word, of
the left word, U+0001, the right word, or of the two words with U+0002;
the fingerprint is its upper 32 bits with the lowest bit set.

The corpora are downloaded into .cache/tekito-data/leipzig-ja; the pack is
written to data/ja/japanese-lm (not committed, like japanese-core).
"""

from __future__ import annotations

import argparse
import hashlib
import re
import json
import math
import shutil
import struct
import tarfile
import urllib.request
from array import array
from collections import Counter
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from datapacks import DATA, manifests, pack_dir  # noqa: E402
CACHE = ROOT / ".cache" / "tekito-data" / "leipzig-ja"
CORPORA = ["jpn_newscrawl_2019_1M", "jpn_wikipedia_2021_1M"]
URL = "https://downloads.wortschatz-leipzig.de/corpora/{}.tar.gz"
# magic, version, pair slots, pairs offset, word slots, words offset, pairs, size, tokens,
# topic slots, topics offset
HEADER = struct.Struct("<4s10I")
MAGIC = b"TKLM"
VERSION = 3
CONTENT = re.compile(r"[\u4E00-\u9FFF\u3005\u30A1-\u30FA\u30FC]")
MIN_SIGNIFICANCE = 6.63  # p < 0.01
PMI_FLOOR = -5.0
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def fnv(text: str) -> int:
    h = FNV_OFFSET
    for byte in text.encode("utf-16-le"):
        h ^= byte
        h = (h * FNV_PRIME) & MASK64
    return h


def fetch(corpus: str) -> Path:
    path = CACHE / f"{corpus}.tar.gz"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {URL.format(corpus)}")
        with urllib.request.urlopen(URL.format(corpus)) as response:
            path.write_bytes(response.read())
    return path


def rows(archive: tarfile.TarFile, name: str):
    with archive.extractfile(name) as source:
        for raw in source:
            yield raw.decode("utf-8", "replace").rstrip("\n").split("\t")


def is_content(word: str) -> bool:
    return len(word) >= 2 and CONTENT.search(word) is not None


def read_topics(corpus: str, topics: dict[tuple[str, str], float]) -> None:
    """The corpus's sentence co-occurrences of content words (the most
    significant of the corpora for each pair)."""
    with tarfile.open(fetch(corpus)) as archive:
        words = {fields[0]: fields[1] for fields in rows(archive, f"{corpus}/{corpus}-words.txt")
                 if len(fields) >= 3 and is_content(fields[1])}
        for fields in rows(archive, f"{corpus}/{corpus}-co_s.txt"):
            if len(fields) < 4 or fields[0] not in words or fields[1] not in words:
                continue
            first, second = words[fields[0]], words[fields[1]]
            if first >= second:
                continue
            significance = float(fields[3])
            if significance >= MIN_SIGNIFICANCE and significance > topics.get((first, second), 0.0):
                topics[(first, second)] = significance


def read_corpus(corpus: str, ids: dict[str, int], counts: array, pairs: Counter) -> int:
    """Adds the corpus's words and neighbor pairs (by global word id)."""
    with tarfile.open(fetch(corpus)) as archive:
        local: dict[int, int] = {}
        tokens = 0
        for fields in rows(archive, f"{corpus}/{corpus}-words.txt"):
            if len(fields) < 3 or not fields[0].isdigit() or not fields[2].isdigit():
                continue
            word = fields[1]
            if word not in ids:
                ids[word] = len(ids)
                counts.append(0)
            local[int(fields[0])] = ids[word]
            counts[ids[word]] += int(fields[2])
            tokens += int(fields[2])

        # Word places: how long each sentence is, then the words in order.
        inverse = f"{corpus}/{corpus}-inv_w.txt"
        lengths: dict[int, int] = {}
        for fields in rows(archive, inverse):
            sentence, place = int(fields[1]), int(fields[2])
            if place + 1 > lengths.get(sentence, 0):
                lengths[sentence] = place + 1
        offsets: dict[int, int] = {}
        total = 0
        for sentence in sorted(lengths):
            offsets[sentence] = total
            total += lengths[sentence]
        words = array("i", [-1]) * total
        for fields in rows(archive, inverse):
            word = local.get(int(fields[0]), -1)
            words[offsets[int(fields[1])] + int(fields[2])] = word
        for sentence, offset in offsets.items():
            for at in range(offset, offset + lengths[sentence] - 1):
                left, right = words[at], words[at + 1]
                if left >= 0 and right >= 0:
                    pairs[(left << 32) | right] += 1
        print(f"{corpus}: {tokens:,} words, {len(lengths):,} sentences")
        return tokens


def table(entries: dict[int, int]) -> tuple[int, bytes, bytes]:
    slots = 1
    while slots < len(entries) / 0.6:
        slots *= 2
    keys = array("I", [0]) * slots
    scores = bytearray(slots)
    for h, score in entries.items():
        fingerprint = ((h >> 32) | 1) & 0xFFFFFFFF
        at = h & (slots - 1)
        while keys[at] != 0:
            at = (at + 1) & (slots - 1)
        keys[at] = fingerprint
        scores[at] = score
    return slots, keys.tobytes(), bytes(scores)


def write_pack(out: Path, kept: dict[int, int], word_scores: dict[int, int], topic_scores: dict[int, int],
               tokens: int, source: str) -> None:
    pair_slots, pair_keys, pair_values = table(kept)
    word_slots, word_keys, word_values = table(word_scores)
    topic_slots, topic_keys, topic_values = table(topic_scores)
    pairs_offset = HEADER.size
    words_offset = pairs_offset + len(pair_keys) + len(pair_values)
    topics_offset = words_offset + len(word_keys) + len(word_values)
    size = topics_offset + len(topic_keys) + len(topic_values)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    with (out / "lm.bin").open("wb") as output:
        output.write(HEADER.pack(MAGIC, VERSION, pair_slots, pairs_offset, word_slots, words_offset, len(kept),
                                 size, tokens, topic_slots, topics_offset))
        output.write(pair_keys + pair_values + word_keys + word_values + topic_keys + topic_values)

    digest = hashlib.sha256((out / "lm.bin").read_bytes()).hexdigest().upper()
    (out / "NOTICE").write_text(f"""Which Japanese words go together, from the Leipzig Corpora Collection:
{source} (1,000,000 sentences each), reduced to word counts
and the pointwise mutual information of neighbor pairs by
scripts/ja/build-japanese-lm.py.

The downloadable corpora are provided under CC BY. Attribution: (c) Universitat
Leipzig / Sachsische Akademie der Wissenschaften / InfAI.
D. Goldhahn, T. Eckart and U. Quasthoff, "Building Large Monolingual
Dictionaries at the Leipzig Corpora Collection", LREC 2012.
Terms: https://www.wortschatz.uni-leipzig.de/en/usage
""", encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-lm",
        "display_name": "Japanese Word Pairs",
        "schema_version": 1,
        "version": "2026.09",
        "language": "ja-JP",
        "type": "japanese-lm",
        "format": "ja-lm-v3",
        "file": "lm.bin",
        "entry_count": len(kept),
        "sha256": {"file": digest},
        "license": "CC-BY-4.0",
        "source": "Leipzig Corpora Collection: " + source,
        "notice_file": "NOTICE",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {out / 'lm.bin'}: {size / 1e6:.1f} MB")


def build_mini(out: Path) -> None:
    """A small pack for the unit tests (tests/data/ja/japanese-lm-mini)."""
    tokens = 10_000_000
    counts = {"麻酔": 1000, "注射": 1000, "駐車": 5000, "よろしく": 2000, "お願い": 3000, "を": 1_000_000}
    pmis = {("よろしく", "お願い"): 5.0}
    topics = {("注射", "麻酔"): 40.0}
    write_pack(out,
               {fnv(left + "\u0001" + right): round((pmi - PMI_FLOOR) * 10) for (left, right), pmi in pmis.items()},
               {fnv(word): round(math.log(count) * 10) for word, count in counts.items()},
               {fnv(first + "\u0002" + second): round(math.log1p(sig) * 20) for (first, second), sig in topics.items()},
               tokens, "a few hand-written counts for the unit tests")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=pack_dir("japanese-lm"))
    parser.add_argument("--min-pair", type=int, default=2, help="pairs seen fewer times are left out")
    parser.add_argument("--min-word", type=int, default=5, help="rarer words are left out")
    parser.add_argument("--mini", action="store_true", help="write the unit tests' small pack to --out")
    args = parser.parse_args()
    if args.mini:
        build_mini(args.out)
        return 0

    ids: dict[str, int] = {}
    counts = array("q")
    pairs: Counter = Counter()
    tokens = sum(read_corpus(corpus, ids, counts, pairs) for corpus in CORPORA)
    words = {word: counts[i] for word, i in ids.items()}
    by_id = {i: word for word, i in ids.items()}

    kept: dict[int, int] = {}
    for key, count in pairs.items():
        if count < args.min_pair:
            continue
        left, right = key >> 32, key & 0xFFFFFFFF
        if counts[left] < args.min_word or counts[right] < args.min_word:
            continue
        pmi = math.log(count * tokens / (counts[left] * counts[right]))
        kept[fnv(by_id[left] + "\u0001" + by_id[right])] = max(1, min(255, round((pmi - PMI_FLOOR) * 10)))
    word_scores = {fnv(word): min(255, round(math.log(count) * 10))
                   for word, count in words.items() if count >= args.min_word}
    topics: dict[tuple[str, str], float] = {}
    for corpus in CORPORA:
        read_topics(corpus, topics)
    topic_scores = {fnv(first + "\u0002" + second): max(1, min(255, round(math.log1p(significance) * 20)))
                    for (first, second), significance in topics.items()}

    write_pack(args.out, kept, word_scores, topic_scores, tokens, ", ".join(CORPORA))
    print(f"{len(kept):,} pairs of {len(pairs):,}, {len(word_scores):,} words, {len(topic_scores):,} topic pairs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
