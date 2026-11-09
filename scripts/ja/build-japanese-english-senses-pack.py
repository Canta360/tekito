"""Builds the japanese-english-senses data pack: the everyday English words
each Japanese word is glossed with (直す: repair, fix, correct; 治す: cure,
heal), from JMdict, for reading Japanese candidates through English with
the english-pairs pack (src/Core/Japanese/EnglishTies.cpp). JMdict glosses
put the common senses first and in plain words, as WordNet's synonym sets
do not (it has no "fix" for 直す).

Each row is "word<TAB>english words, comma-separated", the word as written
(kanji forms, and the kana form of words written in kana), from the first
few senses: the words of glosses of one or two words, "to" taken off
verbs, articles off nouns.

Usage:
  python scripts/ja/build-japanese-english-senses-pack.py --jmdict <JMdict_e.gz>
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from datapacks import pack_dir  # noqa: E402

SENSES = 3
WORDS = 8
ONE_WORD = re.compile(r"^[a-z]+$")


LEFT_OUT = {"of", "for", "in", "on", "at", "to", "with", "by", "and", "or", "one's", "something", "someone", "oneself"}


def gloss_words(gloss: str) -> list[str]:
    """The words of a gloss of one or two words (branch office: branch,
    office), "to" off verbs and articles off nouns."""
    text = re.sub(r"\s*\([^)]*\)", "", gloss.lower()).strip()
    for lead in ("to ", "a ", "an ", "the "):
        if text.startswith(lead):
            text = text[len(lead):]
    parts = text.split()
    if not 1 <= len(parts) <= 2:
        return []
    return [part for part in parts if ONE_WORD.match(part) and part not in LEFT_OUT]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jmdict", type=Path, required=True)
    args = parser.parse_args()

    rows: dict[str, list[str]] = {}
    with gzip.open(args.jmdict, "rb") as source:
        for _, entry in ET.iterparse(source, events=("end",)):
            if entry.tag != "entry":
                continue
            kanji = [k.findtext("keb") for k in entry.findall("k_ele")]
            kana = [r.findtext("reb") for r in entry.findall("r_ele")]
            words: list[str] = []
            for sense in entry.findall("sense")[:SENSES]:
                for gloss in sense.findall("gloss"):
                    for word in gloss_words(gloss.text or ""):
                        if word not in words and len(words) < WORDS:
                            words.append(word)
            if words:
                # Kana forms only for words not written in kanji.
                for form in [k for k in kanji if k] or [r for r in kana if r]:
                    known = rows.setdefault(form, [])
                    for word in words:
                        if word not in known and len(known) < WORDS:
                            known.append(word)
            entry.clear()

    pack = pack_dir("japanese-english-senses")
    pack.mkdir(parents=True, exist_ok=True)
    data = pack / "senses.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as output:
        for form in sorted(rows, key=lambda f: f.encode("utf-8")):
            output.write(f"{form}\t{','.join(rows[form])}\n")
    digest = hashlib.sha256(data.read_bytes()).hexdigest().upper()
    (pack / "NOTICE").write_text(
        "English glosses of Japanese words, taken from JMdict (https://www.edrdg.org/wiki/index.php/JMdict-EDICT_Dictionary_Project),\n"
        "the property of the Electronic Dictionary Research and Development Group, used in conformance with the\n"
        "Group's licence (https://www.edrdg.org/edrdg/licence.html), under Creative Commons Attribution-ShareAlike 4.0\n"
        "(https://creativecommons.org/licenses/by-sa/4.0/). This pack is shared under the same licence.\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-english-senses",
        "display_name": "Japanese Words in English",
        "schema_version": 1,
        "version": "jmdict-2026.10",
        "language": "ja-JP",
        "type": "japanese-english-senses",
        "file": "senses.tsv",
        "entry_count": len(rows),
        "sha256": {"file": digest},
        "license": "CC-BY-SA-4.0",
        "source": "JMdict (EDRDG), English glosses of the first senses",
        "notice_file": "NOTICE",
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"Built {manifest['display_name']}: {len(rows):,} words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
