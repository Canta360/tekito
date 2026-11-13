"""Adds unusual-readings.tsv to a built japanese-jmdict pack: words read in a
way the word is hardly read (店 read たな, 門 read かど), or read as only old
Japanese read it while it is read otherwise today (天 read あめ), from JMdict
(CC BY-SA 4.0, as the pack), KANJIDIC2 and JMnedict (the same).

Both are the same word as a reading of today: some meaning (an English
gloss) of it is one of that reading's too. A word of its own, however old or
rare, is neither (天地 read あめつち, 候 read そうろう: to be; 自重 read
じちょう: prudence, not じじゅう's dead load).

- "rare": no common JMdict entry (ke_pri or re_pri) has the word with the
  reading, while one has the word with another reading, and another word
  with the reading (たな: 棚). A reading in a common entry is common unless
  JMdict marks it irregular, old, rare or search-only (ik, ok, rk, sk):
  霊験 read れいげん and 初産 read しょさん are common. For a word of one
  kanji, only a reading marked common itself is, when another one of the
  entry is (側: がわ, not かわ).
- "archaic": every JMdict sense of the word so read is marked arch or obs,
  while the word has a reading of today.

Not either: a reading JMnedict has the word with as a place (斑鳩 read いかるが),
words with kana in them (forms of words: 降り read おり), and, for a word of
one kanji, a reading compounds use (KANJIDIC2: an on reading, also voiced
or ending in っ, 今: こん, 版: ばん; a kun stem or prefix, 古: ふる; a kun
reading voiced, 川: がわ).

TEKITO makes them cost more for that reading (archaic most), so 雨 and 飴
come before 天 for あめ.

Rows: reading, tab, word, tab, kind ("rare" or "archaic"); sorted.

Usage:
  python scripts/ja/build-japanese-unusual-readings.py --jmdict JMdict_e.gz --jmnedict JMnedict.xml.gz \\
      --kanjidic kanjidic2.xml.gz --pack <japanese-jmdict pack>
"""

from __future__ import annotations

import argparse
import collections
import gzip
import hashlib
import json
import re
from pathlib import Path

FILE = "unusual-readings.tsv"
IRREGULAR = ("&ik;", "&ok;", "&rk;", "&sk;")
VOICED = str.maketrans("がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ", "かきくけこさしすせそたちつてとはひふへほはひふへほ")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def forms(reading: str) -> set[str]:
    plain = reading[:1].translate(VOICED) + reading[1:]
    return {plain, plain[:-1] + "っ" if len(plain) > 1 else plain}


def gloss_key(gloss: str) -> str:
    gloss = re.sub(r"\([^)]*\)", "", gloss).lower()
    return re.sub(r"^(to|a|an|the) ", "", gloss.strip())


def kanji_only(word: str) -> bool:
    return all("一" <= c <= "鿿" or c in "々〆" for c in word)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jmdict", type=Path, required=True)
    parser.add_argument("--jmnedict", type=Path, required=True)
    parser.add_argument("--kanjidic", type=Path, required=True)
    parser.add_argument("--pack", type=Path, required=True)
    args = parser.parse_args()

    common: dict[tuple[str, str], bool] = collections.defaultdict(bool)
    archaic: dict[tuple[str, str], bool] = {}
    glosses: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
    text = gzip.open(args.jmdict, "rt", encoding="utf-8").read()
    for entry in re.findall(r"<entry>(.*?)</entry>", text, re.S):
        kanji = [re.search(r"<keb>(.*?)</keb>", k).group(1) for k in re.findall(r"<k_ele>(.*?)</k_ele>", entry, re.S)]
        entry_common = "<ke_pri>" in entry or "<re_pri>" in entry
        senses = re.findall(r"<sense>(.*?)</sense>", entry, re.S)
        entry_archaic = bool(senses) and all("&arch;" in sense or "&obs;" in sense for sense in senses)
        meanings = {gloss_key(g) for sense in senses for g in re.findall(r"<gloss[^>]*>(.*?)</gloss>", sense)}
        for r in re.findall(r"<r_ele>(.*?)</r_ele>", entry, re.S):
            if "<re_nokanji/>" in r:
                continue
            reading = re.search(r"<reb>(.*?)</reb>", r).group(1)
            reading_common = entry_common and not any(mark in r for mark in IRREGULAR)
            marked_alone = "<re_pri>" in r or "<re_pri>" not in entry
            only = re.findall(r"<re_restr>(.*?)</re_restr>", r)
            for word in kanji:
                if only and word not in only:
                    continue
                key = (word, reading)
                common[key] = common[key] or (reading_common and (len(word) > 1 or marked_alone))
                archaic[key] = archaic.get(key, True) and entry_archaic
                glosses[key] |= meanings

    # Places only: surnames are read every way (天 read あめ too).
    names: set[tuple[str, str]] = set()
    for entry in re.findall(r"<entry>(.*?)</entry>", gzip.open(args.jmnedict, "rt", encoding="utf-8").read(), re.S):
        if "&place;" not in entry and "&station;" not in entry:
            continue
        for word in re.findall(r"<keb>(.*?)</keb>", entry):
            for reading in re.findall(r"<reb>(.*?)</reb>", entry):
                names.add((word, reading))

    compound: dict[str, set[str]] = collections.defaultdict(set)
    plain_kun: dict[str, set[str]] = collections.defaultdict(set)
    kanjidic = gzip.open(args.kanjidic, "rt", encoding="utf-8").read()
    for character in re.findall(r"<character>(.*?)</character>", kanjidic, re.S):
        literal = re.search(r"<literal>(.*?)</literal>", character).group(1)
        for kind, value in re.findall(r'<reading r_type="ja_(on|kun)">(.*?)</reading>', character):
            if kind == "on":
                compound[literal] |= forms(hiragana(value.strip("-")))
            elif "." in value or value.endswith("-"):
                compound[literal].add(value.split(".")[0].strip("-"))
            else:
                plain_kun[literal].add(value.strip("-"))

    def compound_reading(word: str, reading: str) -> bool:
        if len(word) != 1:
            return False
        unvoiced = reading[:1].translate(VOICED) + reading[1:]
        voiced_kun = unvoiced != reading and unvoiced in plain_kun.get(word, set())
        return voiced_kun or bool(forms(reading) & compound.get(word, set()))

    common_readings: dict[str, set[str]] = collections.defaultdict(set)
    common_words: dict[str, set[str]] = collections.defaultdict(set)
    todays_readings: dict[str, set[str]] = collections.defaultdict(set)
    for (word, reading), is_common in common.items():
        if is_common:
            common_readings[word].add(reading)
            common_words[reading].add(word)
        if not archaic[(word, reading)]:
            todays_readings[word].add(reading)

    def same_word(word: str, reading: str, others: set[str]) -> bool:
        return any(glosses[(word, reading)] & glosses[(word, other)] for other in others - {reading})

    rows = []
    for (word, reading), is_common in common.items():
        if not kanji_only(word) or compound_reading(word, reading) or (word, reading) in names:
            continue
        if archaic[(word, reading)] and same_word(word, reading, todays_readings[word]):
            rows.append((reading, word, "archaic"))
        elif not is_common and common_words[reading] - {word} and same_word(word, reading, common_readings[word]):
            rows.append((reading, word, "rare"))
    rows.sort()

    path = args.pack / FILE
    with path.open("w", encoding="utf-8", newline="\n") as out:
        for reading, word, kind in rows:
            out.write(f"{reading}\t{word}\t{kind}\n")
    notice = args.pack / "NOTICE"
    line = (f"{FILE} lists words read in a rare or archaic way, derived from JMdict, JMnedict and KANJIDIC2, "
            "the property of the Electronic Dictionary Research and Development Group (https://www.edrdg.org/), "
            "used in conformance with the Group's licence, under CC BY-SA 4.0.")
    if notice.is_file() and line not in notice.read_text(encoding="utf-8"):
        with notice.open("a", encoding="utf-8", newline="\n") as out:
            out.write("\n" + line + "\n")
    manifest_path = args.pack / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    files = manifest.get("files") or [manifest["file"]]
    if FILE not in files:
        files.append(FILE)
    manifest["files"] = files
    manifest.setdefault("sha256", {})["unusual-readings"] = sha256(path)
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    archaic_count = sum(1 for row in rows if row[2] == "archaic")
    print(f"{len(rows):,} unusual readings ({archaic_count:,} archaic) in {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
