"""Lists Japanese set phrases from Wiktionary (四字熟語, 慣用句, ことわざ,
故事成語, 成句) with their readings, for build-japanese-packs.py
--wikimedia-words: 一石二鳥, 画竜点睛, 猫の手も借りたい.

Reads the English and the Japanese Wiktionary as extracted by wiktextract
(kaikki.org). A reading is kept only when the phrase can be read so with the
readings KANJIDIC2 gives its kanji (build-japanese-wikipedia-words.py).
Written out like the Wikipedia words, kind "phrase":

  reading \t surface \t phrase \t 0

Usage:
  python scripts/ja/build-japanese-wiktionary-phrases.py \\
      --english .cache/tekito-data/raw-wiktextract-data.jsonl.gz \\
      --japanese .cache/tekito-data/jawiktionary-20260901/raw.jsonl.gz \\
      --kanjidic .cache/tekito-data/kanjidic2/kanjidic2.xml.gz \\
      --out .cache/tekito-data/jawiki/phrases.tsv
"""

from __future__ import annotations

import argparse
import gzip
import importlib.util
import json
import re
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "wikimedia_words", Path(__file__).with_name("build-japanese-wikipedia-words.py"))
words = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(words)

# The categories of set phrases, in either Wiktionary.
KINDS = ("yojijukugo", "idioms", "proverbs", "四字熟語", "慣用句", "ことわざ", "故事成語", "成句")
SURFACE = re.compile(rf"[{words.KANJI}ぁ-ゖァ-ヺー]{{2,24}}")
# 漢字（よみ） as the Japanese Wiktionary writes a phrase with its reading.
RUBY = re.compile(rf"([{words.KANJI}]+)[（(]([ぁ-ゖー]+)[）)]")


def categories(entry: dict) -> set[str]:
    found = set(entry.get("categories") or [])
    for sense in entry.get("senses") or []:
        found.update(sense.get("categories") or [])
    return {c if isinstance(c, str) else c.get("name", "") for c in found}


def readings(entry: dict) -> list[str]:
    """The readings an entry gives, in hiragana, likeliest first."""
    out = []
    for template in entry.get("head_templates") or []:
        args = template.get("args") or {}
        name = template.get("name", "")
        for key in (("2", "1") if name == "ja-pos" else ("1",)):
            if key in args:
                out.append(args[key])
    for form in entry.get("forms") or []:
        text = form.get("form", "")
        tags = form.get("tags") or []
        if "transliteration" in tags:
            out.append(text)
        if "canonical" in tags and form.get("ruby"):
            text = entry["word"]
            for kanji, kana in form["ruby"]:
                text = text.replace(kanji, kana, 1)
            out.append(text)
        elif "canonical" in tags and RUBY.search(text):
            out.append(RUBY.sub(r"\2", text))
    cleaned = []
    for text in out:
        text = words.hiragana(re.sub(r"[\s.^\-‐・]", "", text))
        if re.fullmatch(r"[ぁ-ゖー]{2,40}", text) and text not in cleaned:
            cleaned.append(text)
    return cleaned


def phrases(path: Path, language: str, kanji: dict[str, set[str]]):
    marker = f'"lang_code": "{language}"'
    with gzip.open(path, "rt", encoding="utf-8") as stream:
        for line in stream:
            if marker not in line:
                continue
            entry = json.loads(line)
            if entry.get("lang_code") != language:
                continue
            surface = entry.get("word", "")
            if not SURFACE.fullmatch(surface) or not words.HAS_KANJI.search(surface):
                continue
            if not any(kind in c for c in categories(entry) for kind in KINDS):
                continue
            for reading in readings(entry):
                if words.readable(surface, reading, kanji):
                    yield reading, surface
                    break


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--english", type=Path, required=True, help="the English Wiktionary's extract")
    parser.add_argument("--japanese", type=Path, required=True, help="the Japanese Wiktionary's extract")
    parser.add_argument("--kanjidic", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    kanji = words.kanji_readings(args.kanjidic)
    found = set(phrases(args.english, "ja", kanji)) | set(phrases(args.japanese, "ja", kanji))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as out:
        for reading, surface in sorted(found):
            out.write(f"{reading}\t{surface}\tphrase\t0\n")
    print(f"{len(found):,} phrases", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
