#!/usr/bin/env python3
"""Writes the japanese-topics Data Pack from the word vectors
train-japanese-topics.py made: the commonest words' vectors, int8.

vectors.bin, little-endian:
  "TKTV", u32 version (1), u32 count, u32 dim
  then for each row: f32 scale, i8 vector[dim]  (vector / scale = unit vector)
words.tsv: word, row, sorted bytewise by the word (SortedTsv).

  build-japanese-topics-pack.py --vectors words.kv --out data/ja/japanese-topics [--words 200000]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

NOTICE = """Word vectors for the topic of a sentence, trained by TEKITO
(scripts/ja/train-japanese-topics.py) on text from the zenz-v2.5-dataset
(https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset), its
llm-jp-corpus-v3 part, which is made available under the Open Data Commons
Attribution License (ODC-BY 1.0): https://opendatacommons.org/licenses/by/1-0/
The pack holds words and their vectors only; it contains no sentences from
the dataset.
"""


def main() -> int:
    import numpy as np
    from gensim.models import KeyedVectors

    parser = argparse.ArgumentParser()
    parser.add_argument("--vectors", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--words", type=int, default=200000)
    parser.add_argument("--version", default="2026.10")
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    wv = KeyedVectors.load(args.vectors)
    words = wv.index_to_key[:args.words]  # commonest first
    vectors = np.stack([wv[w] for w in words]).astype(np.float32)
    vectors /= np.linalg.norm(vectors, axis=1, keepdims=True)
    scale = np.abs(vectors).max(axis=1) / 127.0
    quantized = np.clip(np.rint(vectors / scale[:, None]), -127, 127).astype(np.int8)
    blob = bytearray(b"TKTV")
    blob += struct.pack("<3I", 1, len(words), vectors.shape[1])
    for row in range(len(words)):
        blob += struct.pack("<f", float(scale[row]))
        blob += quantized[row].tobytes()
    (out / "vectors.bin").write_bytes(bytes(blob))
    rows = sorted(((w.encode("utf-8"), i) for i, w in enumerate(words)), key=lambda r: r[0])
    with open(out / "words.tsv", "wb") as f:
        for word, row in rows:
            f.write(word + b"\t" + str(row).encode() + b"\n")
    (out / "NOTICE").write_text(NOTICE, encoding="utf-8", newline="\n")

    def sha(name: str) -> str:
        return hashlib.sha256((out / name).read_bytes()).hexdigest().upper()

    manifest = {
        "pack_id": "japanese-topics",
        "display_name": "Japanese Topic Vectors",
        "schema_version": 1,
        "version": args.version,
        "language": "ja-JP",
        "type": "japanese-topics",
        "format": "ja-topics-v1",
        "file": "vectors.bin",
        "words_file": "words.tsv",
        "entry_count": len(words),
        "sha256": {"file": sha("vectors.bin"), "words": sha("words.tsv")},
        "license": "TEKITO-OWNED",
        "source": "scripts/ja/train-japanese-topics.py on zenz-v2.5-dataset (ODC-BY 1.0)",
        "notice_file": "NOTICE",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(words)} words, {vectors.shape[1]} dims, {len(blob) / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
