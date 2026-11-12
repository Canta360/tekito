"""Builds the product-names data pack: how the names of software, services,
companies and brands are written (zapier: Zapier, deepl: DeepL), for English
typed among Japanese (Mix), from Wikidata (CC0). Unlike english-names, any
item with an English label and one sitelink or more is in, but only of
those kinds, and the pack only writes words Mix already took as English: it
is not a word list for English typing, where obscure names would hide typos.

A name that is a word of the standard-english lexicon too (Safari, Git,
Redis) is in only as software or an online service whose Japanese Wikipedia
article is titled with it in Latin letters (Safari, Slack (ソフトウェア)), and then only for
Japanese sentences (scope "japanese"): Japanese writes that name so, while a
word such as Hello or Weather stays a word. Two letters are in only when
Japanese text writes them so most of the time (the Leipzig Corpora
Collection's Japanese news 2019 and Wikipedia 2021 corpora, CC BY 4.0).

A name is written so by itself (scope "always") only when it is known:
Japanese writes it so (its Japanese Wikipedia article, or the corpora), or
it has articles in several languages and is no near miss of a common English
word. Any other name (MEETin, one article; meetin is meeting cut short) is
only offered as a candidate after the keys as typed (scope "offer").

The inputs are QLever's (https://qlever.dev/api/wikidata) answers as TSV,
for the classes in ROOT/.cache/tekito-data/wikidata/q_products.rq:
  products_ge1.tsv:
  SELECT ?item ?label ?links (SAMPLE(?class) AS ?type) WHERE {
    VALUES ?class { wd:Q7397 wd:Q1058914 ... } ?item wdt:P31 ?class .
    ?item wikibase:sitelinks ?links . FILTER(?links >= 1)
    ?item rdfs:label ?label . FILTER(LANG(?label) = "en" || LANG(?label) = "mul")
  } GROUP BY ?item ?label ?links
  products_jawiki.tsv:
  SELECT DISTINCT ?item ?title WHERE { VALUES ?class { ... } ?item wdt:P31 ?class .
    ?article schema:about ?item ; schema:isPartOf <https://ja.wikipedia.org/> ; schema:name ?title . }

Usage:
  python scripts/en/build-product-names-pack.py --names products_ge1.tsv --titles products_jawiki.tsv \\
      --lexicon <standard-english/lexicon.txt> --scores <frequency/word-scores.tsv> \\
      --english-names <english-names/entries.tsv> \\
      --japanese <leipzig-ja folder>
"""

from __future__ import annotations

import argparse
import collections
import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# One word: letters and digits, with . + - inside (Node.js, Notepad++).
NAME = re.compile(r"^[A-Za-z][A-Za-z0-9]*(?:[.+-][A-Za-z0-9]+)*\+*$")
JAPANESE_CORPORA = ["jpn_newscrawl_2019_1M", "jpn_wikipedia_2021_1M"]
# Latin words among Japanese (not inside an English phrase).
LATIN = re.compile(r"(?<![A-Za-z0-9])[A-Za-z][A-Za-z0-9.+-]*[A-Za-z0-9+]|(?<![A-Za-z0-9])[A-Za-z](?![A-Za-z0-9])")
KANA = re.compile(r"[぀-ヿ]")
# Shorter names only when Japanese text writes them so: seen this often,
# and written so this often.
SHORTEST = 3
SHORT_SEEN = 2
SHORT_SHARE = 0.6
# A plain word is a name only as software or an online service (not a
# company, brand or website, whose Japanese articles include bands and
# shops named Here, Like or Lush).
PLAIN_KINDS = {
    "Q7397", "Q6368", "Q9135", "Q9143", "Q166142", "Q1639024", "Q19967801", "Q193424", "Q3220391",
    "Q341", "Q506883", "Q2462003", "Q13741", "Q131093", "Q218616", "Q620615", "Q20706915",
    "Q101408242", "Q2115026", "Q29642950", "Q189210", "Q1254596", "Q8076",
}
# Nor a word English uses this often (log10 frequency in word-scores.tsv:
# check 4.3, hello 3.26; safari 2.8, git 2.3), more likely meant as a word.
PLAIN_COMMON = 3.2
# Known: articles in this many languages, or Japanese writes it so.
KNOWN_LINKS = 4
# A near miss of a word English uses this often (log10): one letter more,
# less or other, or its start (meetin: meeting).
NEAR_COMMON = 3.0
NEAR_START = 3


def near_words(scores: dict[str, float]):
    common = {word for word, score in scores.items() if score >= NEAR_COMMON and word.isalpha()}
    starts = {word[:i] for word in common for i in range(NEAR_START, len(word))}
    letters = "abcdefghijklmnopqrstuvwxyz"

    def near(key: str) -> bool:
        if key in starts or key in common:
            return True
        for i in range(len(key) + 1):
            if any(key[:i] + c + key[i:] in common for c in letters):
                return True
        for i in range(len(key)):
            if key[:i] + key[i + 1:] in common:
                return True
            if any(c != key[i] and key[:i] + c + key[i + 1:] in common for c in letters):
                return True
        return False

    return near


def literal(field: str) -> str:
    match = re.match(r'^"(.*)"(?:@[\w-]+)?$', field)
    return match.group(1) if match else field


def japanese_cases(folder: Path) -> dict[str, collections.Counter]:
    counts: dict[str, collections.Counter] = collections.defaultdict(collections.Counter)
    for corpus in JAPANESE_CORPORA:
        path = folder / corpus / f"{corpus}-sentences.txt"
        for line in path.open(encoding="utf-8"):
            text = line.split("\t", 1)[-1]
            if not KANA.search(text):
                continue
            for match in LATIN.finditer(text):
                start, end = match.span()
                # Inside English (a space on both sides): not among Japanese.
                if 0 < start and end < len(text) and text[start - 1] == " " and text[end] == " ":
                    continue
                word = match.group().rstrip(".-")
                counts[word.lower()][word] += 1
    return counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--names", type=Path, required=True)
    parser.add_argument("--titles", type=Path, required=True)
    parser.add_argument("--lexicon", type=Path, required=True)
    parser.add_argument("--scores", type=Path, required=True)
    parser.add_argument("--english-names", type=Path, required=True)
    parser.add_argument("--japanese", type=Path, required=True)
    args = parser.parse_args()

    spec = importlib.util.spec_from_file_location("full", ROOT / "scripts" / "en" / "build-full-data-packs.py")
    full = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(full)

    english = {line.split("\t")[0].strip().lower() for line in open(args.lexicon, encoding="utf-8")
               if line.strip() and not line.startswith("#")}
    known = {line.split("\t")[0] for line in open(args.english_names, encoding="utf-8") if "\t" in line}
    scores: dict[str, float] = {}
    for line in open(args.scores, encoding="utf-8"):
        fields = line.rstrip("\n").split("\t")
        if len(fields) >= 2:
            scores[fields[0]] = float(fields[1])
    cases = japanese_cases(args.japanese)
    # Each item's Japanese Wikipedia title, its parenthesized part left out
    # (Slack (ソフトウェア): Slack).
    titled: dict[str, str] = {}
    with open(args.titles, encoding="utf-8") as f:
        next(f, None)
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) >= 2:
                titled[fields[0]] = re.sub(r"\s*[(（].*[)）]$", "", literal(fields[1])).strip()

    best: dict[str, tuple[str, int, bool, str]] = {}
    with open(args.names, encoding="utf-8") as f:
        next(f, None)
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 3 or not fields[2].isdigit():
                continue
            label = literal(fields[1]).strip()
            links = int(fields[2])
            if not NAME.match(label) or not any(c.isupper() for c in label):
                continue
            key = label.lower()
            if key in known:
                continue
            japanese = titled.get(fields[0]) == label
            kind = fields[3].rsplit("/", 1)[-1].rstrip(">") if len(fields) > 3 else ""
            if key not in best or (japanese, links) > (best[key][2], best[key][1]):
                best[key] = (label, links, japanese, kind)

    near = near_words(scores)
    rows: dict[str, tuple[str, str]] = {}
    plain = offered = 0
    for key, (label, links, japanese, kind) in best.items():
        if len(key) < SHORTEST:
            seen = cases.get(key, collections.Counter())
            total = sum(seen.values())
            if total < SHORT_SEEN or seen.get(label, 0) / total < SHORT_SHARE:
                continue
        if key in english:
            if japanese and kind in PLAIN_KINDS and scores.get(key, 0.0) < PLAIN_COMMON:
                rows[key] = (label, "japanese")
                plain += 1
            continue
        written_so = cases.get(key, collections.Counter()).get(label, 0) >= SHORT_SEEN
        if japanese or written_so or (links >= KNOWN_LINKS and not near(key)):
            rows[key] = (label, "always")
        else:
            rows[key] = (label, "offer")
            offered += 1

    pack = full.reset_pack("product-names")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for key in sorted(rows, key=lambda k: k.encode("utf-8")):
            written, scope = rows[key]
            output.write(f"{key}\t{written}\t{scope}\ten-US\tWikidata product\n")
    manifest = full.finish_pack(
        "product-names", "Product Names from Wikidata", "wikidata-2026.11", "written-forms", "entries.tsv",
        "TEKITO_SLANG_INDEX_V1", 1, "CC0-1.0",
        "https://www.wikidata.org/ (labels of software, services, companies and brands), queried at https://qlever.dev/",
        "How the names of software, services, companies and brands are written, from Wikidata "
        "(https://www.wikidata.org/), by Wikidata contributors, under CC0 1.0 "
        "(https://creativecommons.org/publicdomain/zero/1.0/). Short names were chosen by how often "
        "Japanese text writes them so, counted in the Leipzig Corpora Collection's Japanese news 2019 "
        "and Wikipedia 2021 corpora (CC BY 4.0): D. Goldhahn, T. Eckart and U. Quasthoff, \"Building "
        "Large Monolingual Dictionaries at the Leipzig Corpora Collection\", LREC 2012.")
    print(f"Built {manifest['display_name']}: {manifest['entry_count']:,} entries ({plain:,} plain words, Japanese only; "
          f"{offered:,} only offered)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
