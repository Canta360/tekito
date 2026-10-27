"""Builds the english-names data pack: proper nouns for English typing from
Wikidata (CC0), so names TEKITO does not know (people, companies, brands,
works, places beyond GeoNames') are not taken for typos, and come with
their capitals. Written as proper-nouns is (key, display, proper, en-US,
source; sorted by key, with an index).

Each item's English label with 15 or more sitelinks, all in ASCII letters,
of one to three words that each start with a capital (Boston, Donald Trump,
Kubernetes); and the words of such names on their own (Zelenskyy). A name
or word whose key is an English word of the standard-english lexicon is
left out (Will, Apple), so ordinary words never turn into names.

The input is QLever's (https://qlever.dev/api/wikidata) answer as TSV:
  SELECT ?item ?label ?links (SAMPLE(?class) AS ?type) WHERE {
    ?item wikibase:sitelinks ?links . FILTER(?links >= 15)
    ?item rdfs:label ?label . FILTER(LANG(?label) = "en")
    OPTIONAL { ?item wdt:P31 ?class } } GROUP BY ?item ?label ?links

Usage:
  python scripts/en/build-english-names-pack.py --names english_names.tsv --lexicon <standard-english/lexicon.txt>
"""

from __future__ import annotations

import argparse
import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAME = re.compile(r"^[A-Z][A-Za-z'-]*(?: [A-Z][A-Za-z'-]*){0,2}$")
WORD = re.compile(r"^[A-Z][a-z]+(?:[A-Z][a-z]+)*$")
# A word of a name counts on its own when it is this long and this many
# names have it.
WORD_LENGTH = 4
WORD_NAMES = 1


def literal(field: str) -> str:
    match = re.match(r'^"(.*)"(?:@[\w-]+)?$', field)
    return match.group(1) if match else field


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--names", type=Path, required=True)
    parser.add_argument("--lexicon", type=Path, required=True)
    args = parser.parse_args()

    spec = importlib.util.spec_from_file_location("full", ROOT / "scripts" / "en" / "build-full-data-packs.py")
    full = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(full)

    english = {line.split("\t")[0].strip().lower() for line in open(args.lexicon, encoding="utf-8")
               if line.strip() and not line.startswith("#")}
    names: dict[str, tuple[str, int]] = {}
    words: dict[str, dict[str, int]] = {}
    with open(args.names, encoding="utf-8") as f:
        next(f, None)
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 3 or "/entity/Q" not in fields[0] or not fields[2].isdigit():
                continue
            label = " ".join(literal(fields[1]).split())
            links = int(fields[2])
            if not NAME.match(label):
                continue
            key = label.lower()
            if key not in english and (key not in names or links > names[key][1]):
                names[key] = (label, links)
            for word in label.split():
                if len(word) >= WORD_LENGTH and WORD.match(word) and word.lower() not in english:
                    words.setdefault(word.lower(), {})
                    words[word.lower()][word] = words[word.lower()].get(word, 0) + 1
    for key, forms in words.items():
        display, count = max(forms.items(), key=lambda kv: kv[1])
        if sum(forms.values()) >= WORD_NAMES and key not in names:
            names[key] = (display, 0)

    pack = full.reset_pack("english-names")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for key in sorted(names):
            output.write(f"{key}\t{names[key][0]}\tproper\ten-US\tWikidata name\n")
    manifest = full.finish_pack(
        "english-names", "Names from Wikidata", "wikidata-2026.10", "proper-noun-gazetteer", "entries.tsv",
        "TEKITO_SLANG_INDEX_V1", 1, "CC0-1.0", "https://www.wikidata.org/ (English labels, sitelinks), queried at https://qlever.dev/",
        "Names of people, organizations, companies, brands, works and places, and the words of those names, "
        "from Wikidata (https://www.wikidata.org/), by Wikidata contributors, under CC0 1.0 "
        "(https://creativecommons.org/publicdomain/zero/1.0/).")
    print(f"Built {manifest['display_name']}: {manifest['entry_count']:,} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
