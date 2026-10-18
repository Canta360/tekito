#!/usr/bin/env python3
"""Builds the kanji-sets pack: which kanji are 教育漢字, 常用漢字 and 人名用漢字,
for the setting that limits the kanji candidates may use.

The source is KANJIDIC2 (EDRDG, CC BY-SA 4.0), whose grade field encodes the
sets: 1-6 教育漢字, 8 the rest of 常用漢字, 9 and 10 人名用漢字.

Output: data/ja/kanji-sets/kanji.tsv, "kanji<TAB>level" sorted by code point,
level 1-6 = 教育 of that school year, 8 = 常用 (not 教育), 9 = 人名用.

Usage: python scripts/ja/build-kanji-sets.py [kanjidic2.xml.gz]
(default: .cache/tekito-data/kanjidic2 of the main checkout)
"""

from __future__ import annotations

import gzip
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT.parents[2] if ROOT.parent.name == "worktrees" else ROOT
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else MAIN / ".cache" / "tekito-data" / "kanjidic2" / "kanjidic2.xml.gz"
OUT = ROOT / "data" / "ja" / "kanji-sets"


def main() -> int:
    text = gzip.open(SOURCE, "rt", encoding="utf-8").read()
    version = re.search(r"<database_version>(.*?)</database_version>", text).group(1)
    levels: dict[str, int] = {}
    for literal, body in re.findall(r"<character>\s*<literal>(.*?)</literal>(.*?)</character>", text, re.S):
        grade = re.search(r"<grade>(\d+)</grade>", body)
        if not grade:
            continue
        g = int(grade.group(1))
        level = g if g <= 6 else 8 if g == 8 else 9 if g in (9, 10) else 0
        if level:
            levels[literal] = level
    counts = {level: sum(1 for v in levels.values() if v == level) for level in (1, 2, 3, 4, 5, 6, 8, 9)}
    kyoiku = sum(counts[g] for g in range(1, 7))
    assert kyoiku == 1026 and kyoiku + counts[8] == 2136 and counts[9] == 863, counts
    OUT.mkdir(parents=True, exist_ok=True)
    data = OUT / "kanji.tsv"
    data.write_text("".join(f"{k}\t{levels[k]}\n" for k in sorted(levels)), encoding="utf-8", newline="\n")
    (OUT / "NOTICE").write_text(
        "Which kanji are 教育漢字, 常用漢字 and 人名用漢字, from KANJIDIC2\n"
        "(https://www.edrdg.org/wiki/index.php/KANJIDIC_Project), the property of the\n"
        "Electronic Dictionary Research and Development Group, used under its licence,\n"
        "Creative Commons Attribution-ShareAlike 4.0 International\n"
        "(https://creativecommons.org/licenses/by-sa/4.0/). Only the grade of each\n"
        "kanji is kept, by scripts/ja/build-kanji-sets.py. This pack is under the same\n"
        "licence.\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "kanji-sets",
        "display_name": "Kanji Sets (KANJIDIC2)",
        "schema_version": 1,
        "version": f"kanjidic2-{version}",
        "language": "ja-JP",
        "type": "kanji-sets",
        "file": "kanji.tsv",
        "entry_count": len(levels),
        "sha256": {"file": hashlib.sha256(data.read_bytes()).hexdigest().upper()},
        "license": "CC-BY-SA-4.0",
        "source": "https://www.edrdg.org/kanjidic/kanjidic2.xml.gz",
        "notice_file": "NOTICE",
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                       encoding="utf-8", newline="\n")
    print(f"{len(levels)} kanji: {counts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
