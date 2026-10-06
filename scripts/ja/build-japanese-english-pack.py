"""Builds the japanese-english pack: katakana words and the English they
are written as, both ways (グーグル <-> Google, ミーティング <-> meeting).

Sources, downloaded into .cache/tekito-data:
  edrdg/JMdict_e.gz               loanwords and their English source words
  jawiki/jawiki-latest-*.sql.gz   Japanese Wikipedia titles: a katakana title
                                  and the English edition's title it links
                                  to, and katakana redirects to English titles
  jawiki/intros.jsonl             the opening sentence of articles titled in
                                  Latin letters, which gives the reading
                                  ("GitHub（ギットハブ）は"); fetched by
                                  fetch-wikipedia-intros.py from the list
                                  this script writes, jawiki/latin-titles.json

Files (sorted TSV, UTF-8):
  words.tsv    key (katakana read loosely, as LoanwordKey), English,
               katakana, score; a key's rows best first
  english.tsv  English in lower case without spaces, English, katakana,
               score; best first

The score is how well known the word is: JMdict's common-word marks, or how
many Wikipedia editions have the article.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import re
import shutil
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from datapacks import DATA, ROOT  # noqa: E402

CACHE = ROOT / ".cache" / "tekito-data"
VERSION = "2026.10"
# JMdict's score for a word marked common.
COMMON_LOANWORD = 6.0

KATAKANA = re.compile(r"^[ァ-ヺー・]+$")
ENGLISH = re.compile(r"^[A-Za-z][A-Za-z0-9 .&'+\-!]*$")
VOWELS = {
    "あ": "あかさたなはまやらわがざだばぱぁゃゎ",
    "い": "いきしちにひみりぎじぢびぴぃ",
    "う": "うくすつぬふむゆるぐずづぶぷぅゅゔ",
    "え": "えけせてねへめれげぜでべぺぇ",
    "お": "おこそとのほもよろをごぞどぼぽぉょ",
}


def loanword_key(kana: str) -> str:
    """LoanwordKey in src/Core/Japanese/Loanwords.cpp, with ・ dropped."""
    text = "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in kana.replace("・", ""))
    for spelled, sound in (("てぃ", "ち"), ("でぃ", "じ"), ("ぢ", "じ"), ("づ", "ず"), ("ゔ", "ぶ")):
        text = text.replace(spelled, sound)
    key = ""
    for c in text:
        if c == "ー":
            key += next((v for v, row in VOWELS.items() if key and key[-1] in row), "")
            continue
        key += c
    return key.translate(str.maketrans("ぁぃぅぇぉ", "あいうえお"))


class Pairs:
    """(katakana, English) with the best score seen."""

    def __init__(self) -> None:
        self.scores: dict[tuple[str, str], float] = {}

    def add(self, katakana: str, english: str, score: float) -> None:
        english = re.sub(r"\s+", " ", english).strip()
        if len(katakana.replace("・", "")) < 2 or not KATAKANA.match(katakana) or not ENGLISH.match(english):
            return
        if len(english) > 40:
            return
        pair = (katakana, english)
        if score > self.scores.get(pair, -1.0):
            self.scores[pair] = score


# The first sound of a kana: its consonant, or the vowel itself.
KANA_ROWS = {
    "k": "かきくけこ", "g": "がぎぐげご", "s": "さすせそ", "sh": "し", "z": "ざずぜぞ", "j": "じぢ",
    "t": "たてと", "ch": "ち", "ts": "つ", "d": "だでど", "n": "なにぬねのん", "h": "はひへほ", "f": "ふ",
    "b": "ばびぶべぼゔ", "p": "ぱぴぷぺぽ", "m": "まみむめも", "y": "やゆよ", "r": "らりるれろ", "w": "わを",
    "a": "あ", "i": "い", "u": "う", "e": "え", "o": "お",
}
# Letters an English word may start with for each first sound.
SOUNDS = {
    "k": "kcq", "g": "g", "s": "scxp", "sh": "sc", "z": "zsx", "j": "jgd", "t": "t", "ch": "ct", "ts": "tc",
    "d": "d", "n": "nk", "h": "hw", "f": "fphw", "b": "bv", "p": "p", "m": "m", "y": "yeju", "r": "rlw",
    "w": "wo", "a": "aeiouh", "i": "ieayh", "u": "uoawh", "e": "eaiyh", "o": "oauh",
}


def sounds_alike(katakana: str, english: str) -> bool:
    """Whether the English word starts the way the katakana does
    (ミーティング and meeting; ワタシ is not I)."""
    first = loanword_key(katakana)[:1]
    sound = next((s for s, row in KANA_ROWS.items() if first in row), "")
    return bool(sound) and english[:1].lower() in SOUNDS[sound]


# ---------------------------------------------------------------------------
# JMdict

def add_jmdict(pairs: Pairs) -> int:
    text = gzip.open(CACHE / "edrdg" / "JMdict_e.gz", "rt", encoding="utf-8").read()
    added = 0
    for entry in text.split("<entry>")[1:]:
        readings = re.findall(r"<r_ele>(.*?)</r_ele>", entry, re.S)
        senses = re.findall(r"<sense>(.*?)</sense>", entry, re.S)
        if not readings or not senses:
            continue
        common = "<re_pri>" in entry
        katakana = [re.search(r"<reb>(.*?)</reb>", r).group(1) for r in readings
                    if "&sk;" not in r and "&ik;" not in r and "&ok;" not in r]
        katakana = [k for k in katakana if KATAKANA.match(k)]
        if not katakana:
            continue
        english = []
        for sense in senses:
            if 'ls_wasei="y"' in sense:
                continue
            for lang, word in re.findall(r"<lsource(?: xml:lang=\"(\w+)\")?[^>]*>(.*?)</lsource>", sense):
                if lang in ("", "eng") and word:
                    english.append(word)
        if not english and 'ls_wasei="y"' not in entry and "<lsource" not in entry:
            # No source word given: the first gloss, when it is a word or a
            # name (ミーティング: meeting, グーグル: Google).
            gloss = re.search(r"<gloss>(.*?)</gloss>", senses[0])
            if gloss and len(gloss.group(1).split()) <= 3 and "(" not in gloss.group(1) and \
                    any(sounds_alike(k, gloss.group(1)) for k in katakana):
                english.append(gloss.group(1))
        for word in english[:2]:
            for k in katakana:
                pairs.add(k, word, COMMON_LOANWORD if common else 3.0)
                added += 1
    return added


# ---------------------------------------------------------------------------
# Japanese Wikipedia

ROW = re.compile(r"\((\d+),(-?\d+),'((?:[^'\\]|\\.)*)'(?:,([^()]*?))?\)")


def sql_rows(path: Path, pattern: re.Pattern):
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as source:
        for line in source:
            if line.startswith("INSERT INTO") or line.startswith("("):
                yield from pattern.finditer(line)


def unescape(title: str) -> str:
    return re.sub(r"\\(.)", r"\1", title).replace("_", " ")


def add_wikipedia(pairs: Pairs, least_editions: int) -> int:
    folder = CACHE / "jawiki"
    # page: id, namespace, title, is_redirect, ...
    page = re.compile(r"\((\d+),(-?\d+),'((?:[^'\\]|\\.)*)',(\d),")
    titles: dict[int, str] = {}
    redirects: set[int] = set()
    for row in sql_rows(folder / "jawiki-latest-page.sql.gz", page):
        if row.group(2) != "0":
            continue
        page_id = int(row.group(1))
        titles[page_id] = unescape(row.group(3))
        if row.group(4) == "1":
            redirects.add(page_id)
    print(f"  {len(titles):,} pages, {len(redirects):,} redirects")

    # langlinks: from, lang, title
    link = re.compile(r"\((\d+),'([a-z\-]+)','((?:[^'\\]|\\.)*)'\)")
    editions: dict[int, int] = defaultdict(int)
    english: dict[int, str] = {}
    for row in sql_rows(folder / "jawiki-latest-langlinks.sql.gz", link):
        page_id = int(row.group(1))
        editions[page_id] += 1
        if row.group(2) == "en":
            english[page_id] = unescape(row.group(3))
    print(f"  {len(english):,} pages linked to English")

    def score(page_id: int) -> float:
        return math.log2(1 + editions.get(page_id, 0))

    def plain(title: str) -> str:
        # "Zoom (software)" is written Zoom; titles start with a capital,
        # so IPhone and EBay are iPhone and eBay.
        title = re.sub(r"\s*\(.*?\)\s*$", "", title)
        if re.match(r"^[IE][A-Z][a-z]", title):
            title = title[0].lower() + title[1:]
        return title

    added = 0
    by_title = {title: page_id for page_id, title in titles.items() if page_id not in redirects}
    for page_id, title in titles.items():
        if page_id in redirects:
            continue
        name = plain(title)
        if editions.get(page_id, 0) < least_editions:
            continue
        if KATAKANA.match(name) and page_id in english:
            pairs.add(name, plain(english[page_id]), score(page_id))
            added += 1

    # Articles titled in Latin letters: their readings come from the
    # opening sentence, fetched separately.
    latin = {page_id: [title, editions.get(page_id, 0)] for page_id, title in titles.items()
             if page_id not in redirects and editions.get(page_id, 0) >= least_editions and
             ENGLISH.match(plain(title))}
    (folder / "latin-titles.json").write_text(json.dumps(latin, ensure_ascii=False), encoding="utf-8")
    intros = folder / "intros.jsonl"
    if intros.exists():
        for line in intros.read_text(encoding="utf-8").splitlines():
            page = json.loads(line)
            title, text = page.get("title") or "", page.get("text") or ""
            name = plain(title)
            # The reading: the first katakana in the brackets after the name.
            bracket = re.match(r"\s*" + re.escape(name) + r"[^（(]{0,40}[（(]([^）)]{0,80})", text)
            if not bracket:
                continue
            reading = re.search(r"[ァ-ヺー・]{2,}", bracket.group(1))
            if reading and page.get("id") in latin:
                pairs.add(reading.group(0).strip("・"), name, score(page["id"]))
                added += 1

    # rd: from, namespace, title, ...
    redirect = re.compile(r"\((\d+),(-?\d+),'((?:[^'\\]|\\.)*)'")
    for row in sql_rows(folder / "jawiki-latest-redirect.sql.gz", redirect):
        if row.group(2) != "0":
            continue
        source = titles.get(int(row.group(1)))
        target = unescape(row.group(3))
        target_id = by_title.get(target)
        if not source or target_id is None or editions.get(target_id, 0) < least_editions:
            continue
        name = plain(source)
        if not KATAKANA.match(name):
            continue
        written = plain(target)
        # A redirect may be a nickname (ググレカス for Google): after the
        # article's own name.
        if ENGLISH.match(written):
            pairs.add(name, written, score(target_id) - 1.0)
            added += 1
        elif target_id in english and KATAKANA.match(plain(target)):
            pairs.add(name, plain(english[target_id]), score(target_id) - 1.5)
            added += 1
    return added


# ---------------------------------------------------------------------------

# Leipzig Japanese corpora: how often an English word is written in Latin
# letters inside Japanese text (API often, take hardly ever).
LEIPZIG = ["jpn_newscrawl_2019_1M", "jpn_wikipedia_2021_1M"]
LATIN_WORD = re.compile(r"(?<![A-Za-z0-9])[A-Za-z][A-Za-z0-9+#]*(?:[.\-][A-Za-z0-9]+)*")
JAPANESE_CHAR = re.compile(r"[ぁ-ゖァ-ヺ一-龯]")


def count_latin_words(least: int = 3) -> tuple[dict[str, int], int]:
    """Latin-letter words in the Japanese sentences, lower case, with their
    counts, and the number of sentences read."""
    counts: dict[str, int] = defaultdict(int)
    sentences = 0
    for corpus in LEIPZIG:
        path = CACHE / "leipzig-ja" / corpus / f"{corpus}-sentences.txt"
        if not path.exists():
            continue
        with path.open(encoding="utf-8", errors="replace") as source:
            for line in source:
                text = line.split("\t", 1)[-1]
                if not JAPANESE_CHAR.search(text):
                    continue
                sentences += 1
                for word in LATIN_WORD.findall(text):
                    counts[word.lower()] += 1
    return {word: count for word, count in counts.items() if count >= least}, sentences


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def write_sorted(path: Path, rows: dict[str, list[tuple[float, str, str]]], per_key: int) -> int:
    count = 0
    with path.open("wb") as output:
        for key in sorted(rows, key=lambda k: k.encode("utf-8")):
            for score, english, katakana in sorted(rows[key], key=lambda r: (-r[0], r[1]))[:per_key]:
                output.write(f"{key}\t{english}\t{katakana}\t{score:.2f}\n".encode("utf-8"))
                count += 1
    return count


def main() -> int:
    global CACHE
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=DATA / "ja")
    parser.add_argument("--cache", type=Path, default=CACHE, help="where the sources were downloaded")
    parser.add_argument("--least-editions", type=int, default=20,
                        help="Wikipedia articles in fewer language editions are left out")
    args = parser.parse_args()
    CACHE = args.cache

    pairs = Pairs()
    print(f"JMdict: {add_jmdict(pairs):,} pairs")
    print(f"Wikipedia: {add_wikipedia(pairs, args.least_editions):,} pairs")

    # Wikipedia capitalizes every title: a word JMdict marks common in lower
    # case (サーバー: server) comes before the title (Server). Python stays
    # first where python is not common.
    for (katakana, english), score in list(pairs.scores.items()):
        lower = english.lower()
        if english != lower and english[:1].isupper() and english[1:] == lower[1:] and \
                pairs.scores.get((katakana, lower), 0.0) >= COMMON_LOANWORD:
            pairs.scores[(katakana, english)] = min(score, pairs.scores[(katakana, lower)] - 0.01)

    # One katakana word is one English word first: アイフォーン is iPhone
    # before iPhone 5, データ is data before Data Encryption Standard.
    for (katakana, english), score in list(pairs.scores.items()):
        if " " in english and "・" not in katakana:
            pairs.scores[(katakana, english)] = score - 1.5

    by_key: dict[str, list[tuple[float, str, str]]] = defaultdict(list)
    by_english: dict[str, list[tuple[float, str, str]]] = defaultdict(list)
    for (katakana, english), score in pairs.scores.items():
        by_key[loanword_key(katakana)].append((score, english, katakana))
        by_english[english.lower().replace(" ", "")].append((score, english, katakana))

    pack = args.out / "japanese-english"
    if pack.exists():
        shutil.rmtree(pack)
    pack.mkdir(parents=True)
    words = write_sorted(pack / "words.tsv", by_key, 5)
    english = write_sorted(pack / "english.tsv", by_english, 5)
    # latin.tsv: word in lower case, log10 of how many times per million
    # Japanese sentences it is written in Latin letters.
    latin, sentences = count_latin_words()
    with (pack / "latin.tsv").open("wb") as output:
        for word in sorted(latin, key=lambda w: w.encode("utf-8")):
            per_million = latin[word] * 1e6 / max(sentences, 1)
            output.write(f"{word}\t{math.log10(per_million):.2f}\n".encode("utf-8"))
    print(f"latin: {len(latin):,} words from {sentences:,} Japanese sentences")
    (pack / "NOTICE").write_text("""Katakana words and the English they are written as, built by
scripts/ja/build-japanese-english-pack.py from:

- JMdict (Electronic Dictionary Research and Development Group,
  https://www.edrdg.org/), loanwords and their source words. Used under the
  Creative Commons Attribution-ShareAlike 4.0 license, in conformance with
  the Group's licence (https://www.edrdg.org/edrdg/licence.html).
- Japanese Wikipedia (https://ja.wikipedia.org/) page titles, redirects and
  interlanguage links, from the Wikimedia dumps. Text of Wikipedia is
  available under the Creative Commons Attribution-ShareAlike 4.0 license.
- How often English words are written in Latin letters in Japanese text
  (latin.tsv), counted in the Leipzig Corpora Collection's Japanese news
  crawl 2019 and Wikipedia 2021 corpora, licensed CC BY 4.0: D. Goldhahn,
  T. Eckart and U. Quasthoff, "Building Large Monolingual Dictionaries at
  the Leipzig Corpora Collection", LREC 2012.

This pack is distributed under CC BY-SA 4.0
(https://creativecommons.org/licenses/by-sa/4.0/).
""", encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-english",
        "display_name": "Katakana and English",
        "schema_version": 1,
        "version": VERSION,
        "language": "ja-JP",
        "type": "japanese-english",
        "format": "sorted-tsv-v1",
        "file": "words.tsv",
        "files": ["words.tsv", "english.tsv", "latin.tsv"],
        "entry_count": words,
        "sha256": {"file": sha256(pack / "words.tsv"), "english": sha256(pack / "english.tsv"),
                   "latin": sha256(pack / "latin.tsv")},
        "license": "CC-BY-SA-4.0",
        "source": "JMdict (EDRDG); Japanese Wikipedia titles, redirects, language links and opening sentences; Leipzig Corpora Collection (Japanese news 2019, Wikipedia 2021)",
        "notice_file": "NOTICE",
    }
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    size = sum(p.stat().st_size for p in pack.iterdir()) / 1e6
    print(f"japanese-english: {words:,} katakana rows, {english:,} English rows, {size:.1f} MB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
