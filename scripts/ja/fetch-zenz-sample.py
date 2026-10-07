#!/usr/bin/env python3
"""Fetches an even sample of the zenz-v2.5 kana-kanji conversion dataset
(Miwa-Keita/zenz-v2.5-dataset on Hugging Face): its llm-jp-corpus-v3 part,
ODC-BY, from Common Crawl, about 32 GB of JSON lines

  {"input": katakana reading, "output": text, "left_context": text or null}

The file is ordered by where its text came from, so rather than its start
this takes `--pieces` slices spread evenly over all of it (HTTP ranges),
less the lines a slice cuts through.

  fetch-zenz-sample.py [--pieces 200] [--piece-mb 10] [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from pathlib import Path

URL = ("https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset/resolve/main/"
       "train_llm-jp-corpus-v3.jsonl")
ROOT = Path(__file__).resolve().parents[2]


def size_of(url: str) -> int:
    request = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
    with urllib.request.urlopen(request) as response:
        return int(response.headers["Content-Range"].rsplit("/", 1)[1])


def fetch(url: str, start: int, end: int) -> bytes:
    request = urllib.request.Request(url, headers={"Range": f"bytes={start}-{end}"})
    with urllib.request.urlopen(request) as response:
        return response.read()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pieces", type=int, default=200)
    parser.add_argument("--piece-mb", type=int, default=10)
    parser.add_argument("--out", default=str(ROOT / ".cache" / "tekito-data" / "zenz-dataset"))
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    total = size_of(URL)
    piece = args.piece_mb * 1024 * 1024
    stride = (total - piece) // max(args.pieces - 1, 1)
    target = out / "sample.jsonl"
    kept = 0
    with target.open("w", encoding="utf-8", newline="\n") as f:
        for i in range(args.pieces):
            start = i * stride
            data = fetch(URL, start, start + piece - 1)
            lines = data.split(b"\n")
            # The first and last lines of a slice are cut through.
            for line in lines[1:-1]:
                try:
                    row = json.loads(line)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
                if row.get("input") and row.get("output"):
                    f.write(json.dumps(row, ensure_ascii=False) + "\n")
                    kept += 1
            print(f"piece {i + 1}/{args.pieces} at {start / total:.1%}: {kept} rows", flush=True)
    print(f"wrote {kept} rows to {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
