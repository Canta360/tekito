"""Builds the japanese-romaji data pack: how typed keys become kana.

Each row is  input <TAB> output <TAB> pending : when the keys typed so far
spell `input`, TEKITO writes `output` and keeps `pending` as the start of the
next input ("kk" -> "っ" with "k" pending). The spellings follow what
Microsoft IME accepts, so people can keep their habits.

Usage:
  python scripts/ja/build-japanese-romaji.py
"""

from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACK = ROOT / "data" / "ja" / "japanese-romaji"
VERSION = "2026.09"

VOWELS = "aiueo"

# Consonant rows: prefix -> kana for a, i, u, e, o ("" = no row entry).
ROWS = {
    "": "あいうえお",
    "k": "かきくけこ", "g": "がぎぐげご", "s": "さしすせそ", "z": "ざじずぜぞ",
    "t": "たちつてと", "d": "だぢづでど", "n": "なにぬねの", "h": "はひふへほ",
    "b": "ばびぶべぼ", "p": "ぱぴぷぺぽ", "m": "まみむめも", "r": "らりるれろ",
    "c": "かしくせこ",
}
# Two-character rows written out, one tuple per vowel a, i, u, e, o.
PAIRS = {
    "y": ("や", "い", "ゆ", "いぇ", "よ"),
    "w": ("わ", "うぃ", "う", "うぇ", "を"),
    "wh": ("うぁ", "うぃ", "う", "うぇ", "うぉ"),
    "f": ("ふぁ", "ふぃ", "ふ", "ふぇ", "ふぉ"),
    "v": ("ゔぁ", "ゔぃ", "ゔ", "ゔぇ", "ゔぉ"),
    "q": ("くぁ", "くぃ", "く", "くぇ", "くぉ"),
    "kw": ("くぁ", "くぃ", "くぅ", "くぇ", "くぉ"),
    "gw": ("ぐぁ", "ぐぃ", "ぐぅ", "ぐぇ", "ぐぉ"),
    "sw": ("すぁ", "すぃ", "すぅ", "すぇ", "すぉ"),
    "sh": ("しゃ", "し", "しゅ", "しぇ", "しょ"),
    "j": ("じゃ", "じ", "じゅ", "じぇ", "じょ"),
    "ch": ("ちゃ", "ち", "ちゅ", "ちぇ", "ちょ"),
    "ts": ("つぁ", "つぃ", "つ", "つぇ", "つぉ"),
    "th": ("てゃ", "てぃ", "てゅ", "てぇ", "てょ"),
    "tw": ("とぁ", "とぃ", "とぅ", "とぇ", "とぉ"),
    "dh": ("でゃ", "でぃ", "でゅ", "でぇ", "でょ"),
    "dw": ("どぁ", "どぃ", "どぅ", "どぇ", "どぉ"),
    "x": ("ぁ", "ぃ", "ぅ", "ぇ", "ぉ"),
    "l": ("ぁ", "ぃ", "ぅ", "ぇ", "ぉ"),
}
# Rows with a small ya/yu/yo: prefix -> the kana before it.
YOON = {
    "ky": "き", "gy": "ぎ", "sy": "し", "zy": "じ", "jy": "じ", "ty": "ち", "cy": "ち",
    "dy": "ぢ", "ny": "に", "hy": "ひ", "by": "び", "py": "ぴ", "my": "み", "ry": "り",
    "fy": "ふ", "vy": "ゔ", "qy": "く",
}
SMALL_Y = ("ゃ", "ぃ", "ゅ", "ぇ", "ょ")

EXTRA = {
    "n": "ん", "nn": "ん", "n'": "ん", "xn": "ん",
    "xtu": "っ", "ltu": "っ", "xtsu": "っ", "ltsu": "っ",
    "xya": "ゃ", "xyu": "ゅ", "xyo": "ょ", "lya": "ゃ", "lyu": "ゅ", "lyo": "ょ",
    "xyi": "ぃ", "xye": "ぇ", "lyi": "ぃ", "lye": "ぇ",
    "xwa": "ゎ", "lwa": "ゎ", "xka": "ヵ", "lka": "ヵ", "xke": "ヶ", "lke": "ヶ",
    "wyi": "ゐ", "wye": "ゑ",
    # Symbols. Everything printable becomes its full-width form, as in
    # Microsoft IME; these few have a Japanese form instead.
    "-": "ー", ",": "、", ".": "。", "[": "「", "]": "」", "/": "・", "~": "〜",
    "\\": "￥", '"': "”", "'": "’", "`": "‘",
}
# A doubled consonant is a small tsu followed by that consonant.
DOUBLED = "bcdfghjkmpqrstvwxyz"


def rows() -> dict[str, tuple[str, str]]:
    table: dict[str, tuple[str, str]] = {}

    def add(key: str, output: str, pending: str = "") -> None:
        if key in table and table[key] != (output, pending):
            raise ValueError(f"conflicting rows for {key!r}: {table[key]} vs {(output, pending)}")
        table[key] = (output, pending)

    for prefix, kana in ROWS.items():
        for vowel, output in zip(VOWELS, kana):
            add(prefix + vowel, output)
    for prefix, outputs in PAIRS.items():
        for vowel, output in zip(VOWELS, outputs):
            add(prefix + vowel, output)
    for prefix, head in YOON.items():
        for vowel, small in zip(VOWELS, SMALL_Y):
            add(prefix + vowel, head + small)
    for key, output in EXTRA.items():
        add(key, output)
    for consonant in DOUBLED:
        add(consonant * 2, "っ", consonant)
    add("tch", "っ", "ch")
    for code in range(0x21, 0x7F):
        char = chr(code)
        if char.isalpha() or char in table:
            continue
        add(char, chr(code + 0xFEE0))
    return table


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def write_index(data: Path, index: Path, stride: int = 64) -> int:
    count = 0
    with data.open("rb") as source, index.open("wb") as output:
        output.write(b"TEKITO_JAPANESE_ROMAJI_INDEX_V1\n")
        while True:
            offset = source.tell()
            line = source.readline()
            if not line:
                break
            if count % stride == 0:
                key = line.rstrip(b"\n").split(b"\t", 1)[0]
                output.write(str(offset).encode("ascii") + b"\t" + key + b"\n")
            count += 1
    return count


def main() -> None:
    table = rows()
    if PACK.exists():
        shutil.rmtree(PACK)
    PACK.mkdir(parents=True)
    data = PACK / "romaji.tsv"
    keys = sorted(table, key=lambda key: key.encode("utf-8"))
    with data.open("w", encoding="utf-8", newline="\n") as output:
        for key in keys:
            out, pending = table[key]
            output.write(f"{key}\t{out}\t{pending}\n")
    count = write_index(data, PACK / "romaji.tsv.idx")
    (PACK / "NOTICE").write_text(
        "How typed keys become kana in Japanese input, following the romaji\n"
        "spellings Microsoft IME accepts.\n\n"
        "Copyright 2026 Capitata. Written for TEKITO and covered by the TEKITO\n"
        "source license (LICENSE.md in the TEKITO repository). Generated by\n"
        "scripts/ja/build-japanese-romaji.py; no third-party table is included.\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-romaji",
        "display_name": "Japanese Romaji Table",
        "schema_version": 1,
        "version": VERSION,
        "language": "ja-JP",
        "type": "japanese-romaji",
        "format": "indexed-tsv-v1",
        "file": "romaji.tsv",
        "index_file": "romaji.tsv.idx",
        "entry_count": count,
        "sha256": {"file": sha256(data), "index": sha256(PACK / "romaji.tsv.idx")},
        "license": "TEKITO-OWNED",
        "source": "scripts/ja/build-japanese-romaji.py",
        "notice_file": "NOTICE",
    }
    (PACK / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"japanese-romaji: {count} rows")


if __name__ == "__main__":
    main()
