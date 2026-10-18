#!/usr/bin/env python3
"""Builds the meaning packs for the other language than the word's, so the
candidate window can explain a word in the language Settings is shown in:

  english-meanings-ja  an English word in Japanese: its Japanese words and
                       the Japanese definition of each sense (heart: 心臓、
                       ハート：…)
  japanese-meanings-en a Japanese word in English: the English words and
                       definition of each sense (心臓: heart: the hollow
                       muscular organ…)

Both come from the Japanese WordNet 1.1 (NICT license), which gives each
Princeton WordNet 3.0 synset its Japanese words and definitions, and from
Princeton WordNet 3.0 itself (WordNet license) for the English words of a
synset and the order of an English word's senses (most used first). The
Japanese words of a sense are put in order by their cost in the Mozc
dictionary (common words first).

Each pack is one sorted TSV in the japanese-meanings format, read in place by
binary search:  word <TAB> reading <TAB> sense [<TAB> sense ...]
(the reading is empty here). Rows are in bytewise order of the word.

Usage: python scripts/ja/build-bilingual-meaning-packs.py
The sources are read from .cache/tekito-data (wnjpn-1.1, wordnet/WordNet-3.0);
the packs are written to data/ja (not committed, like japanese-wordnet).
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import shutil
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT.parents[2] / ".cache" / "tekito-data" if (ROOT.parents[2] / ".cache").exists() else ROOT / ".cache" / "tekito-data"
WNJPN = CACHE / "wnjpn-1.1"
PRINCETON = CACHE / "wordnet" / "WordNet-3.0" / "dict"
MOZC = CACHE / "mozc-b9c3fcbd6d76b19649ef572324fa9da2559bc18e" / "dictionary_oss"
VERSION = "2026.10"

MAX_SENSES = 4
MAX_WORDS = 3
MAX_DEFINITION = 80
POS_FILES = {"n": "noun", "v": "verb", "a": "adj", "r": "adv"}


def shorten(text: str, limit: int) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= limit else text[: limit - 1].rstrip("、。,; ") + "…"


def english_gloss(text: str) -> str:
    # The definition, without the examples after it ("; \"...\"").
    return text.split('; "')[0].strip()


def read_princeton():
    """English words of each synset, and each English word's synsets by use."""
    words_of: dict[str, list[str]] = {}
    for pos, name in POS_FILES.items():
        for line in (PRINCETON / f"data.{name}").read_text(encoding="latin-1").splitlines():
            if line.startswith("  "):
                continue
            fields = line.split(" ")
            offset, kind = fields[0], fields[2]
            count = int(fields[3], 16)
            words = [fields[4 + 2 * i].replace("_", " ") for i in range(count)]
            words = [re.sub(r"\(\w+\)$", "", w) for w in words]
            words_of[f"{offset}-{'a' if kind == 's' else kind}"] = words
    senses_of: dict[str, list[tuple[int, int, str]]] = defaultdict(list)
    for pos, name in POS_FILES.items():
        for line in (PRINCETON / f"index.{name}").read_text(encoding="latin-1").splitlines():
            if line.startswith("  "):
                continue
            fields = line.split(" ")
            lemma = fields[0].replace("_", " ")
            synset_count = int(fields[2])
            pointer_count = int(fields[3])
            tagged = int(fields[5 + pointer_count])
            offsets = fields[6 + pointer_count: 6 + pointer_count + synset_count]
            senses_of[lemma].append((tagged, [f"{offset}-{pos}" for offset in offsets]))
    order_of: dict[str, list[str]] = {}
    synset_rank: dict[str, int] = {}
    for lemma, groups in senses_of.items():
        # The part of speech used most in the tagged corpus leads; the others
        # follow two senses later (cat: the animal before the verb).
        groups.sort(key=lambda group: -group[0])
        ranked = []
        for index, (_, synsets) in enumerate(groups):
            ranked += [(rank + 2 * index, synset) for rank, synset in enumerate(synsets)]
        order_of[lemma] = [synset for _, synset in sorted(ranked)]
        for rank, synset in enumerate(order_of[lemma]):
            synset_rank[synset] = min(synset_rank.get(synset, rank), rank)
    # How often a synset's words were tagged in the corpus: the senses of a
    # Japanese word most used in English come first (本: a book, not an oeuvre).
    tagged_of: dict[str, int] = defaultdict(int)
    pos_of = {"1": "n", "2": "v", "3": "a", "4": "r", "5": "a"}
    for line in (PRINCETON / "index.sense").read_text(encoding="latin-1").splitlines():
        fields = line.split(" ")
        if len(fields) >= 4:
            pos = pos_of[fields[0].split("%")[1][0]]
            tagged_of[f"{fields[1]}-{pos}"] += int(fields[3])
    return words_of, order_of, synset_rank, tagged_of


def read_mozc_costs() -> dict[str, int]:
    costs: dict[str, int] = {}
    for path in sorted(MOZC.glob("dictionary*.txt")):
        for line in path.read_text(encoding="utf-8").splitlines():
            fields = line.split("	")
            if len(fields) >= 5:
                cost = int(fields[3])
                if cost < costs.get(fields[4], 1 << 30):
                    costs[fields[4]] = cost
    return costs


def read_wnjpn():
    japanese_words: dict[str, list[str]] = defaultdict(list)
    with gzip.open(WNJPN / "wnjpn-ok.tab.gz", "rt", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 2:
                continue
            word = fields[1].replace("_", " ").strip()
            if word and not re.search(r"[A-Za-z]", word) and word not in japanese_words[fields[0]]:
                japanese_words[fields[0]].append(word)
    english_def: dict[str, str] = {}
    japanese_def: dict[str, str] = {}
    with gzip.open(WNJPN / "wnjpn-def.tab.gz", "rt", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if len(fields) >= 4 and fields[1] == "0":
                english_def.setdefault(fields[0], fields[2])
                if fields[3].strip():
                    japanese_def.setdefault(fields[0], fields[3])
    return japanese_words, english_def, japanese_def


def write_pack(pack_id: str, display_name: str, language: str, rows: dict[str, list[str]], out: Path,
               notice: str) -> None:
    pack = out / pack_id
    if pack.exists():
        shutil.rmtree(pack)
    pack.mkdir(parents=True)
    data = pack / "meanings.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as target:
        for word in sorted(rows, key=lambda w: w.encode("utf-8")):
            target.write("\t".join([word, ""] + rows[word]) + "\n")
    (pack / "NOTICE").write_text(notice.strip() + "\n", encoding="utf-8", newline="\n")
    digest = hashlib.sha256(data.read_bytes()).hexdigest().upper()
    manifest = {
        "pack_id": pack_id,
        "display_name": display_name,
        "schema_version": 1,
        "version": VERSION,
        "language": language,
        "type": "japanese-meanings",
        "format": "sorted-tsv-v1",
        "file": "meanings.tsv",
        "entry_count": len(rows),
        "sha256": {"file": digest},
        "license": "WordNet AND Japanese-WordNet",
        "source": "Princeton WordNet 3.0; Japanese WordNet 1.1 (bond-lab/wnja)",
        "notice_file": "NOTICE",
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"{display_name}: {len(rows):,} words, {data.stat().st_size / 1e6:.1f} MB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "ja")
    args = parser.parse_args()
    words_of, order_of, synset_rank, tagged_of = read_princeton()
    japanese_words, english_def, japanese_def = read_wnjpn()
    costs = read_mozc_costs()
    for words in japanese_words.values():
        words.sort(key=lambda w: costs.get(w, 1 << 20))

    english_rows: dict[str, list[str]] = {}
    for lemma, synsets in order_of.items():
        if not re.fullmatch(r"[a-z][a-z' -]*", lemma):
            continue
        out: list[str] = []
        for synset in synsets:
            words = japanese_words.get(synset, [])[:MAX_WORDS]
            definition = japanese_def.get(synset, "")
            if not words and not definition:
                continue
            text = "、".join(words)
            if definition:
                text = (text + "：" if text else "") + shorten(definition, MAX_DEFINITION)
            if text not in out:
                out.append(text)
            if len(out) >= MAX_SENSES:
                break
        if out:
            english_rows[lemma] = out

    japanese_rows: dict[str, list[str]] = defaultdict(list)
    for synset, words in sorted(japanese_words.items(), key=lambda item: (-tagged_of.get(item[0], 0),
                                                                     synset_rank.get(item[0], 1 << 20))):
        definition = english_def.get(synset, "")
        english = [w for w in words_of.get(synset, []) if w][:MAX_WORDS]
        if not definition and not english:
            continue
        text = ", ".join(english)
        if definition:
            text = (text + ": " if text else "") + shorten(english_gloss(definition), MAX_DEFINITION * 2)
        for word in words:
            if len(japanese_rows[word]) < MAX_SENSES and text not in japanese_rows[word]:
                japanese_rows[word].append(text)

    license_text = (WNJPN / "license.txt").read_text(encoding="utf-8")
    princeton_license = (PRINCETON.parent / "LICENSE").read_text(encoding="latin-1") \
        if (PRINCETON.parent / "LICENSE").exists() else "WordNet 3.0 Copyright 2006 by Princeton University."
    notice = f"""
Word meanings across Japanese and English, from the Japanese WordNet 1.1
(https://bond-lab.github.io/wnja/) and Princeton WordNet 3.0
(https://wordnet.princeton.edu/), shortened by
scripts/ja/build-bilingual-meaning-packs.py.

Japanese WordNet license:

{license_text}

Princeton WordNet license:

{princeton_license}
"""
    write_pack("english-meanings-ja", "English Words in Japanese (WordNet)", "en-US+ja-JP", english_rows, args.out, notice)
    write_pack("japanese-meanings-en", "Japanese Words in English (WordNet)", "ja-JP+en-US", dict(japanese_rows), args.out, notice)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
