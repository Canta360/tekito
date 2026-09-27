#!/usr/bin/env python3
"""Builds the Japanese meaning packs the candidate window shows beside a
Japanese candidate ("機械" -> what a 機械 is):

  japanese-wiktionary  glosses from the Japanese Wiktionary (CC BY-SA 4.0 /
                       GFDL, via kaikki.org's wiktextract dump); first choice
  japanese-wordnet     definitions from the Japanese WordNet 1.1 (NICT
                       license); for words Wiktionary does not have

Each pack is one sorted TSV, read in place by binary search:

  surface <TAB> reading <TAB> sense [<TAB> sense ...]

The reading is hiragana, or empty when the source does not say which
reading the meaning is for. Rows are in bytewise order of the surface.

The sources are downloaded into .cache/tekito-data; the packs are written to
data/ (not committed, like japanese-core).
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import shutil
import urllib.request
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "tekito-data"
DATA = ROOT / "data"
VERSION = "2026.09"

WIKTIONARY_DATE = "20260901"
WIKTIONARY = (CACHE / f"jawiktionary-{WIKTIONARY_DATE}" / "raw.jsonl.gz",
              "https://kaikki.org/jawiktionary/raw-wiktextract-data.jsonl.gz")
WORDNET_URL = "https://github.com/bond-lab/wnja/releases/download/v1.1/"
WORDNET = CACHE / "wnjpn-1.1"
WORDNET_LICENSE = "https://raw.githubusercontent.com/bond-lab/wnja/master/docs/license.txt"

MAX_SENSES = 4
MAX_SENSE_LENGTH = 110
KANA = re.compile(r"^[ぁ-ゟァ-ヿー]+$")
# "あうの漢字表記。", "はしるの漢字表記。" and the like: pointers, not meanings.
POINTER = re.compile(r"[^。]*?の(?:漢字表記|旧字体|異体字|別表記|表記揺れ|誤記)[^。]*。?")
READING_PREFIX = re.compile(r"^[（(]([ぁ-ゟー]+)[)）]\s*")


def fetch(path: Path, url: str) -> Path:
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        with urllib.request.urlopen(url) as response:
            path.write_bytes(response.read())
    return path


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def clean(text: str) -> str:
    text = re.sub(r"\s+", " ", text.replace("\t", " ")).strip()
    if len(text) > MAX_SENSE_LENGTH:
        text = text[:MAX_SENSE_LENGTH - 1].rstrip() + "…"
    return text


class Senses:
    """Meanings by (surface, reading), in the order they were added."""

    def __init__(self) -> None:
        self.rows: dict[tuple[str, str], list[str]] = defaultdict(list)

    def add(self, surface: str, reading: str, senses: list[str]) -> None:
        row = self.rows[(surface, reading)]
        for sense in senses:
            if sense and sense not in row and len(row) < MAX_SENSES:
                row.append(sense)

    def write(self, path: Path) -> int:
        rows = sorted(((s.encode("utf-8"), r.encode("utf-8"), senses)
                       for (s, r), senses in self.rows.items() if senses))
        with path.open("wb") as output:
            for surface, reading, senses in rows:
                output.write(b"\t".join([surface, reading] + [s.encode("utf-8") for s in senses]) + b"\n")
        return len(rows)


# ---------------------------------------------------------------------------
# Wiktionary


def gloss_text(sense: dict) -> tuple[str, str]:
    """The sense's gloss without pointers, and the kana it points to."""
    glosses = [g for g in sense.get("glosses", []) if g.strip()]
    if not glosses or "no-gloss" in sense.get("tags", []):
        return "", ""
    # A lone heading such as "（動詞の連用形に接続し）" is followed by the
    # senses under it; it means nothing by itself.
    if len(glosses) == 1 and re.fullmatch(r"[（(][^）)]*[）)]", glosses[0].strip()):
        return "", ""
    text = "".join(glosses)
    target = ""
    for form in sense.get("form_of", []):
        if KANA.match(form.get("word", "")):
            target = form["word"]
    text = POINTER.sub("", text).strip()
    # "いぬ。": the whole gloss is the word in kana.
    bare = text.rstrip("。")
    if bare and KANA.match(bare) and not target:
        return "", bare
    return clean(text), target


def build_wiktionary() -> tuple[Senses, int]:
    path = fetch(*WIKTIONARY)
    entries = []
    with gzip.open(path, "rt", encoding="utf-8") as source:
        for line in source:
            entry = json.loads(line)
            if entry.get("lang_code") != "ja" or not entry.get("word"):
                continue
            senses, targets = [], []
            for sense in entry.get("senses", []):
                text, target = gloss_text(sense)
                if text:
                    senses.append(text)
                if target:
                    targets.append(target)
            kanji = [f["form"] for f in entry.get("forms") or [] if "kanji" in f.get("tags", [])]
            entries.append((entry["word"], entry.get("pos", ""), senses, targets, kanji))

    by_kana = defaultdict(list)
    for word, pos, senses, _, kanji in entries:
        if KANA.match(word) and senses:
            by_kana[hiragana(word)].append((senses, kanji))

    result = Senses()
    # Kana headwords first: they hold most meanings, with the kanji that
    # write them ("はし" -> 橋, 箸, 端: one entry each).
    for word, pos, senses, _, kanji in entries:
        if not KANA.match(word) or not senses:
            continue
        reading = hiragana(word)
        result.add(word, reading, senses)
        for spelling in kanji:
            if spelling != word:
                result.add(spelling, reading, senses)

    for word, pos, senses, targets, _ in entries:
        if KANA.match(word):
            continue
        # A pointer to the kana headword ("会う" -> "あう"): its entry that
        # lists this spelling, else the one whose glosses use this word's
        # kanji ("人と同じ場所で…会合する" for 会う); ambiguous ones are left
        # to WordNet rather than guessed.
        for target in targets:
            reading = hiragana(target)
            candidates = by_kana.get(reading, [])
            listed = [s for s, kanji in candidates if word in kanji]
            if listed:
                for s in listed:
                    result.add(word, reading, s)
                continue
            if len(candidates) == 1:
                result.add(word, reading, candidates[0][0])
                continue
            kanji_chars = [c for c in word if not KANA.match(c)]
            scored = [(sum(c in "".join(s) for c in kanji_chars), i) for i, (s, _) in enumerate(candidates)]
            best = max(scored, default=(0, -1))
            if best[0] > 0 and sum(1 for score, _ in scored if score == best[0]) == 1:
                result.add(word, reading, candidates[best[1]][0])
        # The kanji headword's own senses; "（はし）…" says which reading.
        for sense in senses:
            match = READING_PREFIX.match(sense)
            if match:
                result.add(word, match.group(1), [sense[match.end():]])
            else:
                result.add(word, "", [sense])
    return result, len(entries)


# ---------------------------------------------------------------------------
# WordNet


def build_wordnet() -> Senses:
    for name in ("wnjpn-ok.tab.gz", "wnjpn-def.tab.gz"):
        fetch(WORDNET / name, WORDNET_URL + name)
    fetch(WORDNET / "license.txt", WORDNET_LICENSE)
    definitions: dict[str, str] = {}
    with gzip.open(WORDNET / "wnjpn-def.tab.gz", "rt", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if len(fields) >= 4 and fields[1] == "0" and fields[3].strip():
                definitions[fields[0]] = fields[3]
    result = Senses()
    with gzip.open(WORDNET / "wnjpn-ok.tab.gz", "rt", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 2 or fields[0] not in definitions:
                continue
            lemma = fields[1].replace("_", " ").strip()
            if not lemma or re.search(r"[A-Za-z]", lemma):
                continue
            text = definitions[fields[0]].strip()
            result.add(lemma, hiragana(lemma) if KANA.match(lemma) else "", [clean(text + ("" if text.endswith("。") else "。"))])
    return result


# ---------------------------------------------------------------------------
# Packs


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def write_pack(pack_id: str, display_name: str, senses: Senses, license_id: str, source: str,
               notice: str, out: Path) -> dict:
    pack = out / pack_id
    if pack.exists():
        shutil.rmtree(pack)
    pack.mkdir(parents=True)
    count = senses.write(pack / "meanings.tsv")
    (pack / "NOTICE").write_text(notice.strip() + "\n", encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": pack_id,
        "display_name": display_name,
        "schema_version": 1,
        "version": VERSION,
        "language": "ja-JP",
        "type": "japanese-meanings",
        "format": "sorted-tsv-v1",
        "file": "meanings.tsv",
        "entry_count": count,
        "sha256": {"file": sha256(pack / "meanings.tsv")},
        "license": license_id,
        "source": source,
        "notice_file": "NOTICE",
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"{display_name}: {count:,} entries, {(pack / 'meanings.tsv').stat().st_size / 1e6:.1f} MB")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=DATA)
    args = parser.parse_args()

    wiktionary, entry_count = build_wiktionary()
    write_pack("japanese-wiktionary", "Japanese Meanings (Wiktionary)", wiktionary, "CC-BY-SA-4.0 OR GFDL-1.3",
               f"kaikki.org Japanese Wiktionary extract, {WIKTIONARY_DATE}", f"""
Word meanings from the Japanese Wiktionary (ja.wiktionary.org), extracted by
wiktextract (kaikki.org, {WIKTIONARY_DATE} dump, {entry_count:,} Japanese entries)
and shortened by scripts/build-japanese-meaning-packs.py.

Wiktionary text is available under the Creative Commons Attribution-ShareAlike
4.0 License (https://creativecommons.org/licenses/by-sa/4.0/) and the GNU Free
Documentation License. This pack, as a modified work, is under CC BY-SA 4.0.
Authors: the contributors to the Japanese Wiktionary (see each page's history
at https://ja.wiktionary.org/).
""", args.out)

    wordnet = build_wordnet()
    license_text = (WORDNET / "license.txt").read_text(encoding="utf-8", errors="replace")
    write_pack("japanese-wordnet", "Japanese Meanings (WordNet)", wordnet, "NICT-Japanese-WordNet",
               "Japanese WordNet 1.1 (https://bond-lab.github.io/wnja/)", f"""
Definitions from the Japanese WordNet 1.1, shortened by
scripts/build-japanese-meaning-packs.py.

{license_text.strip()}
""", args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
