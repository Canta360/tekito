#!/usr/bin/env python3
"""Flatten the QWERTY typo catalog and Wikipedia misspellings pack into one TSV eval corpus."""

from __future__ import annotations

import argparse
import gzip
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data"
CATALOG_PATH = DATA / "qwerty-typo-catalog" / "catalog.jsonl.gz"
MISSPELLINGS_PATH = DATA / "wikipedia-common-misspellings" / "misspellings.tsv"
DEFAULT_OUTPUT = Path(__file__).resolve().parent.parent / "eval" / "generated" / "eval_corpus.tsv"

COLUMNS = ("source", "split", "category", "typo_operation", "expected_action",
           "raw_token", "expected_token", "left_context", "right_context")


def clean(value: str) -> str:
    return value.replace("\t", " ").replace("\n", " ").replace("\r", " ").strip()


def catalog_rows():
    if not CATALOG_PATH.exists():
        print(f"warning: {CATALOG_PATH} not found, skipping", file=sys.stderr)
        return
    with gzip.open(CATALOG_PATH, "rt", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError as error:
                print(f"warning: {CATALOG_PATH}:{line_number}: {error}", file=sys.stderr)
                continue
            yield {
                "source": "qwerty_catalog",
                "split": entry.get("split", ""),
                "category": entry.get("category", ""),
                "typo_operation": entry.get("typo_operation", ""),
                "expected_action": entry.get("expected_action", ""),
                "raw_token": entry.get("raw_token", ""),
                "expected_token": entry.get("expected_token", ""),
                "left_context": entry.get("left_context", ""),
                "right_context": entry.get("right_context", ""),
            }


def misspelling_rows():
    if not MISSPELLINGS_PATH.exists():
        print(f"warning: {MISSPELLINGS_PATH} not found, skipping", file=sys.stderr)
        return
    with MISSPELLINGS_PATH.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            line = line.rstrip("\n").rstrip("\r")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) != 2:
                print(f"warning: {MISSPELLINGS_PATH}:{line_number}: expected 2 columns, got {len(parts)}",
                      file=sys.stderr)
                continue
            misspelling, correction = parts
            yield {
                "source": "wikipedia_misspelling",
                "split": "test",
                "category": "real_world_misspelling",
                "typo_operation": "",
                "expected_action": "auto_apply_eligible",
                "raw_token": misspelling,
                "expected_token": correction,
                "left_context": "",
                "right_context": "",
            }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)

    count = 0
    with args.output.open("w", encoding="utf-8", newline="\n") as out:
        out.write("\t".join(COLUMNS) + "\n")
        for row in catalog_rows():
            out.write("\t".join(clean(row[column]) for column in COLUMNS) + "\n")
            count += 1
        for row in misspelling_rows():
            out.write("\t".join(clean(row[column]) for column in COLUMNS) + "\n")
            count += 1

    print(f"wrote {count} rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
