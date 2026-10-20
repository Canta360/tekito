"""Lists the words of Japanese Wikipedia article titles with their readings,
for build-japanese-packs.py --wikipedia-words.

An article's opening sentence gives its title's reading
("'''大政翼賛会'''（たいせいよくさんかい）は、…"). A reading is kept only
when the title can be read so with the readings KANJIDIC2 gives its kanji
(on, kun and name readings, with the sound changes of compounds), so a
misplaced parenthesis gives no word. Written out:

  reading \t surface \t kind \t language editions

kind is "person" (the parenthesis gives a birth year), "noun" (the sentence
defines the word: "…とは"), or "proper-noun"; language editions is how many
other Wikipedias have the article, for how often the word comes up.

Usage:
  python scripts/ja/build-japanese-wikipedia-words.py \\
      --dump .cache/tekito-data/jawiki/jawiki-latest-pages-articles.xml.bz2 \\
      --langlinks .cache/tekito-data/jawiki/jawiki-latest-langlinks.sql.gz \\
      --kanjidic .cache/tekito-data/kanjidic2/kanjidic2.xml.gz \\
      --out .cache/tekito-data/jawiki/words.tsv
"""

from __future__ import annotations

import argparse
import bz2
import collections
import gzip
import html
import re
import sys
import xml.etree.ElementTree as ET
from functools import lru_cache
from pathlib import Path

KANJI = r"㐀-䶿一-鿿豈-﫿々〆ヶ"
SURFACE = re.compile(rf"[{KANJI}ぁ-ゖァ-ヺー]{{2,16}}")
HAS_KANJI = re.compile(rf"[{KANJI}]")
# The bold title and the parenthesis after it.
OPENING = re.compile(r"'''([^'（）]{1,40})'''』?\s*（([^（）]{1,200})）")
# How companies and other bodies are named around their names.
AFFIXES = re.compile(r"^(?:株式会社|有限会社|合同会社|一般社団法人|公益社団法人|一般財団法人|公益財団法人|"
                     r"独立行政法人|国立研究開発法人|特定非営利活動法人|学校法人|社会福祉法人|医療法人|宗教法人)|"
                     r"(?:株式会社|有限会社|合同会社)$")
READING = re.compile(r"[ぁ-ゖァ-ヺー]+")
# Titles of lists and overviews, not words.
LISTS = re.compile(r"(?:一覧|年表|の歴史)$")
YEAR = re.compile(r"\d{3,4}年")
DROPPED = str.maketrans("", "", " 　・=＝")

SMALL = {"つ": "っ", "く": "っ", "き": "っ", "ち": "っ"}
VOICED = dict(zip("かきくけこさしすせそたちつてとはひふへほ",
                  "がぎぐげござじずぜぞだぢづでどばびぶべぼ"))
VOICED.update(zip("はひふへほ", "ぱぴぷぺぽ"))


def hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def kanji_readings(path: Path) -> dict[str, set[str]]:
    readings: dict[str, set[str]] = {}
    with gzip.open(path) as stream:
        for _, element in ET.iterparse(stream):
            if element.tag != "character":
                continue
            literal = element.findtext("literal")
            found = set()
            for node in element.iter():
                if node.tag == "reading" and node.get("r_type") in ("ja_on", "ja_kun") or node.tag == "nanori":
                    text = hiragana((node.text or "").split(".")[0].strip("-"))
                    if READING.fullmatch(text):
                        found.add(text)
            if literal and found:
                readings[literal] = found
            element.clear()
    return readings


def variants(reading: str, first: bool, last: bool) -> set[str]:
    """The ways a kanji's reading sounds inside a compound."""
    out = {reading}
    if not first and reading[0] in VOICED:
        out.add(VOICED[reading[0]] + reading[1:])
        if reading[0] in "はひふへほ":
            out.add("ぱぴぷぺぽ"["はひふへほ".index(reading[0])] + reading[1:])
    if not last and len(reading) > 1 and reading[-1] in SMALL:
        out |= {v[:-1] + "っ" for v in list(out)}
    return out


def readable(surface: str, reading: str, kanji: dict[str, set[str]]) -> bool:
    """Whether the surface reads as `reading`, a kanji at a time; one kanji
    may read freely (熟字訓, names), in up to three kana, when three or more
    are there."""
    count = sum(1 for c in surface if HAS_KANJI.match(c))

    @lru_cache(maxsize=None)
    def walk(i: int, j: int, free: int) -> bool:
        if i == len(surface):
            return j == len(reading)
        c = surface[i]
        if not HAS_KANJI.match(c) or c == "ヶ":
            kana = hiragana(c)
            options = {"が", "か", "け"} if c == "ヶ" else {kana}
            return any(reading.startswith(o, j) and walk(i + 1, j + len(o), free) for o in options)
        base = c
        if c == "々" and i > 0:
            base = surface[i - 1]
        for r in kanji.get(base, ()):
            for v in variants(r, i == 0, i == len(surface) - 1):
                if reading.startswith(v, j) and walk(i + 1, j + len(v), free):
                    return True
        if free and count >= 3:
            for k in range(1, 4):
                if j + k <= len(reading) and "ー" not in reading[j:j + k] and walk(i + 1, j + k, free - 1):
                    return True
        return False

    return walk(0, 0, 1)


def plain(line: str) -> str:
    """A line of wikitext as read: no references or templates, links as
    their text."""
    line = re.sub(r"<ref[^>]*/>|<ref[^>]*>.*?</ref>|<[^>]*>", "", line)
    for _ in range(3):
        line = re.sub(r"\{\{[^{}]*\}\}", "", line)
    return re.sub(r"\[\[(?:[^\]|]*\|)?([^\]]*)\]\]", r"\1", line)


def opening_word(title: str, lines: list[str], kanji: dict[str, set[str]]):
    """The bold word of the opening sentence, its reading and kind."""
    for raw in lines:
        if "'''" not in raw or raw.startswith(("{", "|", "!", "*", "#", ":")):
            continue
        line = plain(html.unescape(raw))
        match = OPENING.search(line)
        if not match:
            continue
        bold = match.group(1).translate(DROPPED)
        inside = match.group(2)
        head = re.split(r"[、,，;；:：]", inside, maxsplit=1)[0].strip()
        if "・" not in bold:
            head = head.split("・")[0]
        reading = hiragana(head.translate(DROPPED))
        if not re.fullmatch(r"[ぁ-ゖー]{2,30}", reading):
            return None
        for surface in (bold, AFFIXES.sub("", bold)):
            if not (SURFACE.fullmatch(surface) and HAS_KANJI.search(surface)) or LISTS.search(surface):
                continue
            if surface not in title and title not in surface:
                continue
            if not readable(surface, reading, kanji):
                continue
            if YEAR.search(inside):
                return surface, reading, "person"
            if "とは" in line[match.end():match.end() + 8]:
                return surface, reading, "noun"
            return surface, reading, "proper-noun"
        return None
    return None


def language_counts(path: Path) -> collections.Counter:
    counts: collections.Counter = collections.Counter()
    pattern = re.compile(rb"^\((\d+),'")
    with gzip.open(path) as stream:
        for line in stream:
            counts.update(int(m) for m in pattern.findall(line))
    return counts


def pages(path: Path):
    """(page id, title, the first lines of the text) of each article."""
    title = page_id = None
    article = False
    lines: list[str] = []
    in_text = False
    with bz2.open(path, "rt", encoding="utf-8") as stream:
        for line in stream:
            s = line.strip()
            if s.startswith("<title>"):
                title, page_id, article, lines, in_text = html.unescape(s[7:-8]), None, False, [], False
            elif s.startswith("<ns>"):
                article = s == "<ns>0</ns>"
            elif s.startswith("<id>") and page_id is None:
                page_id = int(s[4:-5])
            elif s.startswith("<redirect"):
                article = False
            elif article and not in_text and "<text" in s:
                lines = [s.split(">", 1)[1] if ">" in s else ""]
                if "</text>" in s or s.endswith("/>"):
                    yield page_id, title, lines
                    article = False
                else:
                    in_text = True
            elif in_text:
                if "</text>" in s or len(lines) > 400:
                    in_text = False
                    yield page_id, title, lines
                    article = False
                else:
                    lines.append(s)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dump", type=Path, required=True)
    parser.add_argument("--langlinks", type=Path, required=True)
    parser.add_argument("--kanjidic", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    kanji = kanji_readings(args.kanjidic)
    languages = language_counts(args.langlinks)
    print(f"{len(kanji):,} kanji, {len(languages):,} pages with other editions", flush=True)
    found: dict[tuple[str, str], tuple[str, int]] = {}
    seen = kept = 0
    for page_id, title, lines in pages(args.dump):
        seen += 1
        if seen % 200000 == 0:
            print(f"{seen:,} articles, {kept:,} words", flush=True)
        title = re.sub(r"\s*[（(][^）)]*[）)]$", "", title).translate(DROPPED)
        if not HAS_KANJI.search(title):
            continue
        word = opening_word(title, lines, kanji)
        if not word:
            continue
        surface, reading, kind = word
        count = languages.get(page_id, 0)
        key = (reading, surface)
        if key not in found or found[key][1] < count:
            found[key] = (kind, count)
            kept += 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8", newline="\n") as out:
        for (reading, surface), (kind, count) in sorted(found.items()):
            out.write(f"{reading}\t{surface}\t{kind}\t{count}\n")
    print(f"{seen:,} articles, {len(found):,} words", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
