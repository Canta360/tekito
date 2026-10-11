"""Turns readings files (source, id, reading, text) into keys files for
`tekito_ja_eval --keys-corpus ... --write-sentences`, so the whole-sentence
lists a reranker learns from come the way typing makes them.

Sentences any --exclude file holds (any tab-separated field, so keys and
corpus TSVs and plain text all work) are left out, so nothing measured is
learned; the files are only read, never shown.

  python scripts/ja/make-reranker-keys.py --out DIR --shards 24
      [--exclude FILE ...] [--exclude-dir DIR ...] [--exclude-sha1 FILE ...]
      READINGS.tsv ...
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
from pathlib import Path


def load_kana_to_keys():
    path = Path(__file__).resolve().parents[2] / "eval" / "ja" / "kana-to-keys.py"
    spec = importlib.util.spec_from_file_location("kana_to_keys", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_excluded(paths: list[str]) -> set[str]:
    excluded: set[str] = set()
    for path in paths:
        for line in Path(path).read_text(encoding="utf-8-sig", errors="replace").splitlines():
            if line.startswith("{"):
                try:
                    row = json.loads(line)
                    excluded.update(v.strip() for v in row.values() if isinstance(v, str) and len(v.strip()) > 1)
                    continue
                except json.JSONDecodeError:
                    pass
            # Any field may be a sentence: leaving out too much costs little.
            excluded.update(f.strip() for f in line.split("\t") if len(f.strip()) > 1)
    return excluded


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--shards", type=int, default=24)
    parser.add_argument("--exclude", action="append", default=[])
    # Every .tsv, .txt and .jsonl under the folder.
    parser.add_argument("--exclude-dir", action="append", default=[])
    parser.add_argument("--exclude-sha1", action="append", default=[])
    parser.add_argument("readings", nargs="+")
    args = parser.parse_args()
    # SHA-1s of UTF-8 sentences to leave out, one per line: held-out text
    # is never read itself, only its hashes.
    hashes = set()
    for path in args.exclude_sha1:
        hashes.update(l.strip().lower() for l in open(path, encoding="utf-8") if l.strip())
    for folder in args.exclude_dir:
        args.exclude += [str(p) for p in sorted(Path(folder).rglob("*"))
                         if p.is_file() and p.suffix in (".tsv", ".txt", ".jsonl")]

    k2k = load_kana_to_keys()
    romaji = k2k.load_romaji()
    marks = {".": "。", ",": "、", "!": "！", "?": "？", "-": "ー"}
    excluded = read_excluded(args.exclude)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    shards = [open(out / f"train_keys_{i:02}.tsv", "w", encoding="utf-8", newline="\n") for i in range(args.shards)]
    kept = skipped = left_out = 0
    seen: set[str] = set()
    for path in args.readings:
        for line in open(path, encoding="utf-8"):
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 4:
                continue
            source, row_id, reading, text = fields[:4]
            if text in excluded or (hashes and any(
                    hashlib.sha1(part.encode("utf-8")).hexdigest() in hashes
                    for part in [text, *(p for p in re.split(r"(?<=[。！？.!?])", text) if p)])):
                left_out += 1
                continue
            if text in seen:
                continue
            seen.add(text)
            keys = k2k.to_keys(reading)
            if not keys:
                skipped += 1
                continue
            # The keys must read back as the reading (marks at the end aside).
            letters = keys.rstrip(".,!?")
            read = romaji.kana(letters) if letters else ""
            if read is None or read + "".join(marks[c] for c in keys[len(letters):]) != k2k.hiragana(reading):
                skipped += 1
                continue
            shards[kept % args.shards].write(f"{source}\t{row_id}\t{keys}\t{text}\n")
            kept += 1
    for shard in shards:
        shard.close()
    print(f"kept {kept}, not typeable {skipped}, left out as measured {left_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
