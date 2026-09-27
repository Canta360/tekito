#!/usr/bin/env python3
"""Builds the Japanese conversion eval corpus from two public sets.

- Mozc's evaluation.tsv (BSD-3-Clause): the rows whose first conversion
  must be a given text ("Conversion Expected").
- AJIMEE-Bench (azooKey; the JWTD v2 based items are CC BY-SA 3.0): 200
  readings with every acceptable conversion, half of them with the text
  before them.

The sources are downloaded at pinned commits into .cache/tekito-data and
flattened into eval/generated/japanese_eval.tsv (not committed):

  source <TAB> id <TAB> context <TAB> reading <TAB> expected [<TAB> expected ...]

Readings are hiragana, as TEKITO's composer gives them to the converter.
"""

from __future__ import annotations

import argparse
import json
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "tekito-data"
OUTPUT = ROOT / "eval" / "generated" / "japanese_eval.tsv"
MOZC_COMMIT = "b9c3fcbd6d76b19649ef572324fa9da2559bc18e"
AJIMEE_COMMIT = "401666cd56d1a570c2021798b64b6da4396bfd45"
SOURCES = {
    "mozc": (CACHE / f"mozc-{MOZC_COMMIT}" / "dictionary_oss" / "evaluation.tsv",
             f"https://raw.githubusercontent.com/google/mozc/{MOZC_COMMIT}/src/data/dictionary_oss/evaluation.tsv"),
    "ajimee": (CACHE / f"ajimee-{AJIMEE_COMMIT[:8]}" / "evaluation_items.json",
               f"https://raw.githubusercontent.com/azooKey/AJIMEE-Bench/{AJIMEE_COMMIT}/JWTD_v2/v1/evaluation_items.json"),
}


def fetch(name: str) -> Path:
    path, url = SOURCES[name]
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        with urllib.request.urlopen(url) as response:
            path.write_bytes(response.read())
    return path


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def clean(text: str) -> str:
    return text.replace("\t", " ").replace("\n", " ").replace("\r", " ").strip()


def mozc_rows():
    for number, line in enumerate(fetch("mozc").read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("\t")
        if line.startswith("#") or len(fields) < 5 or fields[3] != "Conversion Expected":
            continue
        yield "mozc", str(number), "", fields[1], [fields[4]]


def ajimee_rows():
    for item in json.loads(fetch("ajimee").read_text(encoding="utf-8")):
        yield ("ajimee", item["index"], item.get("context_text", ""), hiragana(item["input"]),
               item["expected_output"])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with args.output.open("w", encoding="utf-8", newline="\n") as out:
        for rows in (mozc_rows(), ajimee_rows()):
            for source, row_id, context, reading, expected in rows:
                fields = [source, row_id, clean(context), clean(reading)] + [clean(e) for e in expected]
                out.write("\t".join(fields) + "\n")
                count += 1
    print(f"wrote {count} rows to {args.output}")


if __name__ == "__main__":
    main()
