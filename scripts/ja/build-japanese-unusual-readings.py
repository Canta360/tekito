"""Adds unusual-readings.tsv to a built japanese-jmdict pack: words read in
a way their kanji are hardly read (天 read あめ, 餌 read え, 工夫 read こうふ),
from JMdict's priority marks (CC BY-SA 4.0, as the pack).

A reading of a word is unusual when no JMdict entry that has the word with
that reading is marked common at all (ke_pri or re_pri; an entry marked for
町 makes 街 read まち usual too), while another reading of the same word is
(天: てん) and another word of the same reading (あめ: 雨, 飴). A word of
one kanji read as KANJIDIC2 (CC BY-SA 4.0) gives an on reading (今: こん,
版: ばん) or the stem of a kun reading with okurigana or a prefix (古: ふる,
真: ま), or a kun reading voiced (川: がわ) is not: those are how compounds
are read. Only a kun reading of its own is (天: あめ). Words with kana in
them (降り read おり) are forms of words JMdict lists otherwise: never.
TEKITO makes such a word cost more for that reading, so 飴 comes before 天
for あめ.

An archaic reading of a word that is read otherwise today (天 read あめ:
every JMdict sense of it marked arch or obs, while 天 read てん is not) is
"archaic": TEKITO puts it after the words of today. A word that is itself
archaic (天地 read あめつち, an entry of its own) is neither.

Rows: reading, tab, word, tab, kind ("rare" or "archaic"); sorted.

Usage:
  python scripts/ja/build-japanese-unusual-readings.py --jmdict JMdict_e.gz --kanjidic kanjidic2.xml.gz \
      --pack <japanese-jmdict pack>
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


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jmdict", type=Path, required=True)
    parser.add_argument("--kanjidic", type=Path, required=True)
    parser.add_argument("--pack", type=Path, required=True)
    args = parser.parse_args()

    text = gzip.open(args.jmdict, "rt", encoding="utf-8").read()
    common: dict[tuple[str, str], bool] = collections.defaultdict(bool)
    # Whether every entry with the word so read is archaic in every sense.
    archaic: dict[tuple[str, str], bool] = {}
    for entry in re.findall(r"<entry>(.*?)</entry>", text, re.S):
        kanji = [re.search(r"<keb>(.*?)</keb>", k).group(1) for k in re.findall(r"<k_ele>(.*?)</k_ele>", entry, re.S)]
        entry_common = "<ke_pri>" in entry or "<re_pri>" in entry
        senses = re.findall(r"<sense>(.*?)</sense>", entry, re.S)
        entry_archaic = bool(senses) and all("&arch;" in sense or "&obs;" in sense for sense in senses)
        for r in re.findall(r"<r_ele>(.*?)</r_ele>", entry, re.S):
            if "<re_nokanji/>" in r:
                continue
            reading = re.search(r"<reb>(.*?)</reb>", r).group(1)
            # A reading of its own in a common entry (あま for 天) counts by its own mark.
            reading_common = entry_common and ("<re_pri>" in r or "<re_pri>" not in entry)
            only = re.findall(r"<re_restr>(.*?)</re_restr>", r)
            for word in kanji:
                if only and word not in only:
                    continue
                common[(word, reading)] = common[(word, reading)] or reading_common
                archaic[(word, reading)] = archaic.get((word, reading), True) and entry_archaic

    common_readings: dict[str, set[str]] = collections.defaultdict(set)
    common_words: dict[str, set[str]] = collections.defaultdict(set)
    for (word, reading), is_common in common.items():
        if is_common:
            common_readings[word].add(reading)
            common_words[reading].add(word)
    # Readings of single kanji that compounds use: on readings (voiced too,
    # and with っ for their last kana) and kun stems and prefixes.
    def hiragana(text: str) -> str:
        return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)
    voiced = str.maketrans("がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ", "かきくけこさしすせそたちつてとはひふへほはひふへほ")
    def forms(reading: str) -> set[str]:
        plain = reading[:1].translate(voiced) + reading[1:]
        return {plain, plain[:-1] + "っ" if len(plain) > 1 else plain}
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
        voiced_kun = reading[:1] != reading[:1].translate(voiced) and             reading[:1].translate(voiced) + reading[1:] in plain_kun.get(word, set())
        return voiced_kun or bool(forms(reading) & compound.get(word, set()))

    def kanji_only(word: str) -> bool:
        return all("一" <= c <= "鿿" or c in "々〆" for c in word)

    # Readings of today: in an entry not all archaic.
    todays_readings: dict[str, set[str]] = collections.defaultdict(set)
    for (word, reading), old in archaic.items():
        if not old:
            todays_readings[word].add(reading)

    rows = []
    for (word, reading), is_common in common.items():
        if not kanji_only(word) or compound_reading(word, reading):
            continue
        if archaic[(word, reading)] and todays_readings[word] - {reading}:
            rows.append((reading, word, "archaic"))
        elif not is_common and common_readings[word] - {reading} and common_words[reading] - {word}:
            rows.append((reading, word, "rare"))
    rows.sort()

    path = args.pack / FILE
    with path.open("w", encoding="utf-8", newline="\n") as out:
        for reading, word, kind in rows:
            out.write(f"{reading}\t{word}\t{kind}\n")
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
