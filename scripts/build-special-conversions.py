"""Builds the special-conversions data pack: what TEKITO offers besides words.

rules.tsv has four fields on each row:  section <TAB> language <TAB> key <TAB> value.
Rows that share a section, language and key keep their order.

  word     a date or time word and what it means: day+1 (tomorrow), time,
           year-1, month+0 ...
  format   for each kind (day, time, year, month), the ways to write it:
           {Y} {M} {MM} {D} {DD} {Month} {W} (weekday) {E} {EY} (era, its
           year) {H} {HH} {I} (12-hour) {N} {NN} (minute) {P} (AM/PM)
  name     month and weekday names, AM/PM, the kanji for numbers
  era      Japanese eras and the day each began
  symbol   symbols by reading (Japanese, from Mozc's symbol table) or by
           their ASCII spelling (English, "->" -> "→")
  emoticon Japanese emoticons (kaomoji) by reading, from Mozc's emoticon
           table; "かおもじ" lists them all
  kanji    the single kanji read a way, all in one value, from Mozc's
           single-kanji table (offered after a reading's words)

The dates, formats, names and English symbols are written here for TEKITO.
The Japanese symbols, emoticons and single kanji come from Mozc
(src/data/symbol/symbol.tsv, src/data/emoticon/emoticon.tsv and
src/data/single_kanji/single_kanji.tsv, BSD-3-Clause).

Usage:
  python scripts/build-special-conversions.py --mozc-symbols <symbol.tsv> \\
      --mozc-emoticons <emoticon.tsv> --mozc-single-kanji <single_kanji.tsv> \\
      --mozc-license <Mozc LICENSE> --mozc-commit <sha>
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "data" / "special-conversions"
VERSION = "2026.09"

WORDS = {
    "ja": [
        ("きょう", "day+0"), ("ほんじつ", "day+0"),
        ("あした", "day+1"), ("あす", "day+1"), ("みょうにち", "day+1"),
        ("あさって", "day+2"), ("みょうごにち", "day+2"), ("しあさって", "day+3"),
        ("きのう", "day-1"), ("さくじつ", "day-1"),
        ("おととい", "day-2"), ("おとつい", "day-2"), ("いっさくじつ", "day-2"),
        ("さきおととい", "day-3"),
        ("いま", "time"), ("じこく", "time"),
        ("ことし", "year+0"), ("こんねん", "year+0"), ("ほんねん", "year+0"),
        ("らいねん", "year+1"), ("さらいねん", "year+2"),
        ("きょねん", "year-1"), ("さくねん", "year-1"), ("おととし", "year-2"),
        ("こんげつ", "month+0"), ("らいげつ", "month+1"), ("さらいげつ", "month+2"),
        ("せんげつ", "month-1"), ("せんせんげつ", "month-2"),
    ],
    "en": [
        ("today", "day+0"), ("tomorrow", "day+1"), ("yesterday", "day-1"),
        ("now", "time"),
    ],
}

FORMATS = {
    "ja": {
        "day": ["{Y}/{MM}/{DD}", "{Y}年{M}月{D}日", "{M}月{D}日", "{M}月{D}日({W})",
                "{E}{EY}年{M}月{D}日", "{Y}-{MM}-{DD}", "{W}曜日"],
        "time": ["{H}:{NN}", "{H}時{N}分", "{P}{I}時{N}分"],
        "year": ["{Y}年", "{E}{EY}年", "{Y}"],
        "month": ["{M}月", "{Y}年{M}月"],
    },
    "en": {
        "day": ["{Month} {D}, {Y}", "{M}/{D}/{Y}", "{Y}-{MM}-{DD}", "{W}, {Month} {D}",
                "{D} {Month} {Y}"],
        "time": ["{I}:{NN} {P}", "{H}:{NN}"],
    },
}

NAMES = {
    "ja": {
        "weekday": list("日月火水木金土"),
        "ampm": ["午前", "午後"],
        "era-first": ["元"],
        "kanji-digits": list("〇一二三四五六七八九"),
        "kanji-units": list("十百千"),
        "kanji-groups": list("万億兆京"),
        "daiji-digits": list("零壱弐参四伍六七八九"),
        "daiji-units": list("拾百阡"),
        "daiji-groups": list("萬億兆京"),
    },
    "en": {
        "month": ["January", "February", "March", "April", "May", "June", "July",
                  "August", "September", "October", "November", "December"],
        "weekday": ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                    "Saturday"],
        "ampm": ["AM", "PM"],
    },
}

ERAS = [("明治", "1868-10-23"), ("大正", "1912-07-30"), ("昭和", "1926-12-25"),
        ("平成", "1989-01-08"), ("令和", "2019-05-01")]

# ASCII spellings of symbols, as word processors turn them (English).
ENGLISH_SYMBOLS = [
    ("(c)", "©"), ("(C)", "©"), ("(r)", "®"), ("(R)", "®"), ("(tm)", "™"), ("(TM)", "™"),
    ("->", "→"), ("<-", "←"), ("<->", "↔"), ("=>", "⇒"), ("<=>", "⇔"),
    ("<=", "≤"), ("<=", "⇐"), (">=", "≥"), ("!=", "≠"), ("=/=", "≠"), ("~=", "≈"),
    ("+-", "±"), ("...", "…"), ("--", "–"), ("--", "—"), ("---", "—"),
    ("<<", "«"), (">>", "»"),
    ("1/2", "½"), ("1/3", "⅓"), ("2/3", "⅔"), ("1/4", "¼"), ("3/4", "¾"),
]


# Rows whose first reading names a group of rare characters (radicals,
# hentaigana, nasal kana, Ainu kana): they come up only by that name, not
# by the kana they are also listed under (こ would offer 𛀸 and こ゚).
NAMED_GROUPS = {"ぶしゅ", "へんたいがな", "びだくおん", "あいぬ"}


def mozc_symbols(path: Path) -> list[tuple[str, str]]:
    """(reading, symbol) from Mozc's symbol.tsv, in its order."""
    rows: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    lines = path.read_text(encoding="utf-8").splitlines()
    for line in lines[1:]:  # the first line names the columns
        fields = line.split("\t")
        if len(fields) < 3 or not fields[1] or fields[0].startswith("#"):
            continue
        symbol = fields[1]
        if any(ord(c) < 0x20 for c in symbol):
            continue
        readings = fields[2].split(" ")
        if readings[0] in NAMED_GROUPS:
            readings = readings[:1]
        for reading in readings:
            if reading and (reading, symbol) not in seen:
                seen.add((reading, symbol))
                rows.append((reading, symbol))
    return rows


# Every emoticon also comes up by this reading, as in Mozc.
ALL_EMOTICONS = "かおもじ"


def mozc_emoticons(path: Path) -> list[tuple[str, str]]:
    """(reading, emoticon) from Mozc's emoticon.tsv, in its order."""
    rows: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    everything: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines()[1:]:  # column names first
        fields = line.split("\t")
        if len(fields) < 2 or not fields[0].strip() or fields[0].startswith("#"):
            continue
        face = fields[0]
        if any(ord(c) < 0x20 for c in face):
            continue
        if face not in everything:
            everything.append(face)
        for reading in fields[1].split(" "):
            if reading and (reading, face) not in seen:
                seen.add((reading, face))
                rows.append((reading, face))
    rows += [(ALL_EMOTICONS, face) for face in everything if (ALL_EMOTICONS, face) not in seen]
    return rows


def mozc_single_kanji(path: Path) -> list[tuple[str, str]]:
    """(reading, its kanji as one string) from Mozc's single_kanji.tsv."""
    rows: list[tuple[str, str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 2 and fields[0] and fields[1] and not fields[0].startswith("#"):
            rows.append((fields[0], fields[1]))
    return rows


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mozc-symbols", type=Path, required=True, help="Mozc src/data/symbol/symbol.tsv")
    parser.add_argument("--mozc-emoticons", type=Path, required=True, help="Mozc src/data/emoticon/emoticon.tsv")
    parser.add_argument("--mozc-single-kanji", type=Path, required=True,
                        help="Mozc src/data/single_kanji/single_kanji.tsv")
    parser.add_argument("--mozc-license", type=Path, required=True, help="Mozc LICENSE")
    parser.add_argument("--mozc-commit", required=True, help="the Mozc commit both files are from")
    args = parser.parse_args()

    rows: list[tuple[str, str, str, str]] = []
    for language, words in WORDS.items():
        rows += [("word", language, word, rule) for word, rule in words]
    for language, kinds in FORMATS.items():
        rows += [("format", language, kind, pattern) for kind, patterns in kinds.items() for pattern in patterns]
    for language, names in NAMES.items():
        rows += [("name", language, name, value) for name, values in names.items() for value in values]
    rows += [("era", "ja", name, start) for name, start in ERAS]
    rows += [("symbol", "en", key, symbol) for key, symbol in ENGLISH_SYMBOLS]
    rows += [("symbol", "ja", reading, symbol) for reading, symbol in mozc_symbols(args.mozc_symbols)]
    rows += [("emoticon", "ja", reading, face) for reading, face in mozc_emoticons(args.mozc_emoticons)]
    rows += [("kanji", "ja", reading, kanji) for reading, kanji in mozc_single_kanji(args.mozc_single_kanji)]
    for row in rows:
        if any("\t" in field or "\n" in field or not field for field in row):
            raise ValueError(f"bad row: {row!r}")

    PACK.mkdir(parents=True, exist_ok=True)
    data = PACK / "rules.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as out:
        out.write("# section\tlanguage\tkey\tvalue (scripts/build-special-conversions.py)\n")
        for row in rows:
            out.write("\t".join(row) + "\n")

    license_text = args.mozc_license.read_text(encoding="utf-8")
    (PACK / "NOTICE").write_text(
        "What TEKITO offers besides words: dates and times for words like\n"
        "\"today\" and \"きょう\", symbols by reading or ASCII spelling, and the\n"
        "kanji for numbers.\n\n"
        "The dates, formats, names and English symbols were written for TEKITO\n"
        "and are covered by the TEKITO source license (LICENSE.md in the TEKITO\n"
        "repository).\n\n"
        "The Japanese symbols, emoticons, single kanji and their readings are\n"
        f"from Mozc (https://github.com/google/mozc, commit {args.mozc_commit},\n"
        "src/data/symbol, src/data/emoticon and src/data/single_kanji), under\n"
        "the following license:\n\n"
        + license_text,
        encoding="utf-8", newline="\n")

    manifest = {
        "pack_id": "special-conversions",
        "display_name": "Special Conversions",
        "schema_version": 1,
        "version": VERSION,
        "language": "en-US+ja-JP",
        "type": "special-conversions",
        "format": "rules-tsv-v1",
        "file": "rules.tsv",
        "entry_count": len(rows),
        "sha256": {"file": sha256(data)},
        "license": "TEKITO-OWNED + BSD-3-Clause (Mozc)",
        "source": "scripts/build-special-conversions.py; Japanese symbols, emoticons and single kanji from "
                  f"https://github.com/google/mozc/tree/{args.mozc_commit}/src/data",
        "notice_file": "NOTICE",
    }
    (PACK / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"special-conversions: {len(rows)} rows")


if __name__ == "__main__":
    main()
