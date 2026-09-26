#!/usr/bin/env python3
"""Build TEKITO's redistributable offline data packs from downloaded sources."""

from __future__ import annotations

import collections
import argparse
import gzip
import hashlib
import json
import math
import re
import shutil
import sqlite3
import sys
import unicodedata
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / ".cache" / "tekito-data"
DATA = ROOT / "data"
ASCII_WORD = re.compile(r"^[A-Za-z]+(?:['-][A-Za-z]+)*$")
ASCII_TERM = re.compile(r"^[A-Za-z][A-Za-z .'-]*$")
TOKEN = re.compile(r"[A-Za-z]+(?:'[A-Za-z]+)?")

CORRECTION_OVERRIDES = {
    ("fontrier", "fontier"): ("fontrier", "frontier"),
    ("mediciney", "mediciny"): ("mediciney", "medicine"),
    ("manouverability", "manoeuverability"): ("manouverability", "manoeuvrability"),
    ("phonecian", "phoenecian"): ("phonecian", "phoenician"),
    ("vigeur", "vigueur"): ("vigeur", "vigour"),
    ("unitesstates", "unitedstates"): None,
    # This is a semantic replacement, not a spelling correction.
    ("muhammadan", "muslim"): None,
}

# Source correction targets omitted by the SCOWL-derived lexicon. These were
# individually reviewed against the local English Wiktionary dump; the HTML
# term is also present in the WHATWG HTML Standard.
MANUAL_STANDARD_ADDITIONS = {
    "anthropomorphization", "archimedean", "cellpadding", "compatibilities",
    "correctors", "differentiations", "disputandum", "drumless", "endoliths",
    "extremophile", "futhark", "futhorc", "geometers", "hydrophile",
    "hydrophobe", "interpretor", "johannine", "mccarthyist", "milieux",
    "millennialism", "octahedra", "papanicolaou", "parallelly", "premillennial",
    "premonstratensians", "prolegomena", "repartition", "resignment",
    "unmaneuverable", "unmanoeuvrable",
}


def clean(value: str) -> str:
    return " ".join(value.replace("\t", " ").replace("\r", " ").replace("\n", " ").split())


def require(path: Path) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"required source is missing: {path}")
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def write_index(data_path: Path, index_path: Path, header: str, fields: int,
                stride: int = 1024) -> int:
    rows = 0
    previous = b""
    with data_path.open("rb") as source, index_path.open("wb") as output:
        output.write((header + "\n").encode("ascii"))
        while True:
            offset = source.tell()
            line = source.readline()
            if not line:
                break
            if not line.strip() or line.startswith(b"#"):
                continue
            key = b"\t".join(line.rstrip(b"\r\n").split(b"\t", fields)[:fields])
            if previous and key < previous:
                raise ValueError(f"data is not sorted at {data_path}: {key!r}")
            previous = key
            if rows % stride == 0:
                output.write(str(offset).encode("ascii") + b"\t" + key + b"\n")
            rows += 1
    if rows == 0:
        raise ValueError(f"pack would be empty: {data_path}")
    return rows


def finish_pack(pack_id: str, display_name: str, version: str, pack_type: str,
                filename: str, index_header: str, index_fields: int, license_id: str,
                source: str, notice: str, source_version: str = "") -> dict:
    pack = DATA / pack_id
    data_path = pack / filename
    index_path = pack / f"{filename}.idx"
    count = write_index(data_path, index_path, index_header, index_fields)
    (pack / "NOTICE").write_text(notice.rstrip() + "\n", encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": pack_id,
        "display_name": display_name,
        "schema_version": 1,
        "version": version,
        "language": "en-US",
        "type": pack_type,
        "file": filename,
        "index_file": index_path.name,
        "entry_count": count,
        "sha256": {"file": sha256(data_path), "index": sha256(index_path)},
        "license": license_id,
        "source": source,
        "notice_file": "NOTICE",
    }
    if source_version:
        manifest["source_version"] = source_version
    (pack / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                         encoding="utf-8", newline="\n")
    return manifest


def reset_pack(pack_id: str) -> Path:
    pack = DATA / pack_id
    if pack.exists():
        shutil.rmtree(pack)
    pack.mkdir(parents=True)
    return pack


def build_wikipedia() -> dict:
    source_path = require(CACHE / "wikipedia-common-misspellings.json")
    parsed = json.loads(source_path.read_text(encoding="utf-8"))["parse"]
    rows: dict[tuple[str, str], tuple[str, str]] = {}
    for line in parsed["wikitext"].splitlines():
        match = re.match(r"^\s*([^#<>{}\[\]|]+?)->(.+?)\s*$", line)
        if not match:
            continue
        raw = match.group(1).strip().lower()
        if not ASCII_WORD.fullmatch(raw):
            continue
        for candidate in match.group(2).split(","):
            candidate = candidate.strip()
            if ASCII_WORD.fullmatch(candidate):
                override = CORRECTION_OVERRIDES.get((raw, candidate.lower()), (raw, candidate))
                if override is not None:
                    key = (override[0].lower(), override[1].lower())
                    previous = rows.get(key)
                    if previous is None or (
                        override[1][:1].isupper() and not previous[1][:1].isupper()
                    ):
                        rows[key] = override
    pack = reset_pack("wikipedia-common-misspellings")
    with (pack / "misspellings.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for raw, candidate in sorted(rows.values()):
            output.write(f"{raw}\t{candidate}\n")
    revision = str(parsed["revid"])
    return finish_pack(
        "wikipedia-common-misspellings", "Wikipedia Common Misspellings", revision,
        "misspelling-correction", "misspellings.tsv",
        "TEKITO_WIKIPEDIA_MISSPELLING_INDEX_V1", 1, "CC-BY-SA-4.0",
        "https://en.wikipedia.org/wiki/Wikipedia:Lists_of_common_misspellings/For_machines",
        "Derived from Wikipedia:Lists of common misspellings/For machines, revision "
        f"{revision}. Copyright belongs to the page contributors. This derived pack is "
        "distributed under CC BY-SA 4.0. Source history: "
        f"https://en.wikipedia.org/w/index.php?title=Wikipedia:Lists_of_common_misspellings/For_machines&oldid={revision}\n"
        "License: https://creativecommons.org/licenses/by-sa/4.0/\n"
        "The source page states that this machine-readable list can be out of date; "
        "treat it as a supplemental correction source, not a complete English lexicon.\n"
        "TEKITO normalized arrows to TSV, split alternatives, removed non-word records, "
        "lowercased lookup keys, corrected six audited source typos, removed one "
        "space-dependent target and one semantic replacement, deduplicated case variants, "
        "sorted, and indexed the result.",
        f"oldid-{revision}")


def complete_standard_english() -> dict:
    """Add independently corroborated Wikipedia correction targets to SCOWL."""
    standard_path = DATA / "standard-english" / "lexicon.txt"
    misspelling_path = DATA / "wikipedia-common-misspellings" / "misspellings.tsv"
    frequency_path = DATA / "frequency" / "word-scores.tsv"
    pronunciation_path = DATA / "pronunciation" / "pronunciations.tsv"
    dictionary_path = DATA / "dictionary-display" / "entries.tsv"
    for path in (standard_path, misspelling_path, frequency_path,
                 pronunciation_path, dictionary_path):
        require(path)

    words = {line.strip().lower() for line in standard_path.read_text(encoding="utf-8").splitlines()
             if line.strip()}
    corrections = set()
    with misspelling_path.open(encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if len(fields) == 2 and ASCII_WORD.fullmatch(fields[1].lower()):
                corrections.add(fields[1].lower())

    def first_fields(path: Path, field: int = 0) -> set[str]:
        result = set()
        with path.open(encoding="utf-8") as source:
            for line in source:
                fields = line.rstrip("\n").split("\t")
                if len(fields) > field and fields[field]:
                    result.add(fields[field].lower())
        return result

    corroboration = (
        first_fields(frequency_path) |
        first_fields(pronunciation_path) |
        first_fields(dictionary_path, 1)
    )
    additions = sorted(((corrections & corroboration) | MANUAL_STANDARD_ADDITIONS) - words)
    words.update(additions)
    ordered = sorted(words)
    standard_path.write_text("\n".join(ordered) + "\n", encoding="utf-8", newline="\n")
    index_path = standard_path.with_name(standard_path.name + ".idx")
    write_index(standard_path, index_path, "TEKITO_LEXICON_INDEX_V1", 1, stride=256)

    frequency_rows = {}
    with frequency_path.open(encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split("\t")
            if len(fields) == 2 and fields[0]:
                frequency_rows[fields[0]] = fields[1]
    missing_frequency = sorted(set(ordered) - set(frequency_rows))
    for word in missing_frequency:
        frequency_rows[word] = "0.100000"
    if missing_frequency:
        frequency_path.write_text(
            "\n".join(f"{word}\t{frequency_rows[word]}" for word in sorted(frequency_rows)) + "\n",
            encoding="utf-8", newline="\n")
    frequency_index_path = frequency_path.with_name(frequency_path.name + ".idx")
    write_index(frequency_path, frequency_index_path, "TEKITO_FREQUENCY_INDEX_V1", 1)
    frequency_manifest_path = frequency_path.parent / "manifest.json"
    frequency_manifest = json.loads(frequency_manifest_path.read_text(encoding="utf-8"))
    frequency_manifest["entry_count"] = len(frequency_rows)
    frequency_manifest["sha256"] = {
        "file": sha256(frequency_path),
        "index": sha256(frequency_index_path),
    }
    frequency_manifest_path.write_text(
        json.dumps(frequency_manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n")

    manifest_path = standard_path.parent / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["entry_count"] = len(ordered)
    manifest["version"] = "2026.09.08+validated-corrections-2"
    manifest["sha256"] = {"file": sha256(standard_path), "index": sha256(index_path)}
    manifest["license"] = "SCOWL-permission-terms + CC-BY-SA-4.0"
    manifest["supplemental_source"] = (
        "https://en.wikipedia.org/wiki/Wikipedia:Lists_of_common_misspellings/For_machines"
    )
    manifest["supplemental_source_version"] = "oldid-1199637275"
    manifest["supplemental_criteria"] = (
        "Correction targets also present in the local frequency, pronunciation, or "
        "WordNet dictionary-display pack, plus individually reviewed targets listed in "
        "build-full-data-packs.py."
    )
    manifest["supplemental_manual_review"] = (
        "30 correction targets verified against the local English Wiktionary dump "
        "enwiktionary-2026-08-05; "
        "cellpadding is additionally verified against the WHATWG HTML Standard."
    )
    manifest["supplemental_manual_review_count"] = len(MANUAL_STANDARD_ADDITIONS)
    manifest["supplemental_manual_review_terms"] = sorted(MANUAL_STANDARD_ADDITIONS)
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                             encoding="utf-8", newline="\n")
    notice_path = standard_path.parent / "NOTICE"
    notice = notice_path.read_text(encoding="utf-8")
    if "Audited supplemental standard additions" not in notice:
        notice_path.write_text(
            notice.rstrip() +
            "\n\nSupplemental validated correction targets were derived from Wikipedia's "
            "common-misspellings list, revision 1199637275, and included only when "
            "corroborated by TEKITO's local frequency, pronunciation, or WordNet data.\n"
            "Source: https://en.wikipedia.org/wiki/Wikipedia:Lists_of_common_misspellings/For_machines\n"
            "License: https://creativecommons.org/licenses/by-sa/4.0/\n\n"
            "Audited supplemental standard additions were individually checked against "
            "the local English Wiktionary dump. The technical term 'cellpadding' is also "
            "listed by the WHATWG HTML Standard.\n",
            encoding="utf-8", newline="\n")
    print(f"Standard English completion: added {len(additions)} validated correction targets; "
          f"frequency coverage added {len(missing_frequency)} words")
    return manifest


def leipzig_paths() -> tuple[Path, Path, Path]:
    base = CACHE / "leipzig" / "eng_news_2025_1M"
    return (require(base / "eng_news_2025_1M-words.txt"),
            require(base / "eng_news_2025_1M-co_n.txt"),
            require(base / "eng_news_2025_1M-sentences.txt"))


def leipzig_notice(kind: str) -> str:
    return (f"{kind} derived from the Leipzig Corpora Collection English News 2025 "
            "1,000,000 sentence download. The downloadable corpus is provided under CC BY.\n"
            "Attribution: © 2026 Universität Leipzig / Sächsische Akademie der "
            "Wissenschaften / InfAI.\n"
            "Source: https://wortschatz.uni-leipzig.de/en/download/eng\n"
            "Terms: https://www.wortschatz.uni-leipzig.de/en/usage\n"
            "TEKITO lowercased ASCII word tokens, aggregated counts, normalized scores, "
            "sorted records, and generated a sparse local index.")


def read_leipzig_words(path: Path) -> tuple[dict[int, str], dict[str, int]]:
    by_id: dict[int, str] = {}
    counts: dict[str, int] = collections.defaultdict(int)
    with path.open(encoding="utf-8") as source:
        for line in source:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 3 or not ASCII_WORD.fullmatch(parts[1]):
                continue
            word = parts[1].lower()
            by_id[int(parts[0])] = word
            counts[word] += int(parts[2])
    return by_id, counts


def build_frequency(by_id: dict[int, str], counts: dict[str, int]) -> dict:
    for word in (DATA / "standard-english" / "lexicon.txt").read_text(encoding="utf-8").splitlines():
        counts.setdefault(word.lower(), 0)
    pack = reset_pack("frequency")
    with (pack / "word-scores.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for word in sorted(counts):
            score = 0.1 if counts[word] == 0 else 1.0 + math.log10(counts[word])
            output.write(f"{word}\t{score:.6f}\n")
    return finish_pack("frequency", "Word Frequency", "eng-news-2025-1M",
                       "ranking-frequency-priors", "word-scores.tsv",
                       "TEKITO_FREQUENCY_INDEX_V1", 1, "CC-BY-4.0", 
                       "https://downloads.wortschatz-leipzig.de/corpora/eng_news_2025_1M.tar.gz",
                       leipzig_notice("Word-frequency data"), "2025-1M")


def build_phrase(sentence_path: Path) -> dict:
    database = CACHE / "phrase-build.sqlite3"
    database.unlink(missing_ok=True)
    db = sqlite3.connect(database)
    db.executescript("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=FILE;"
                     "CREATE TABLE n(context TEXT, word TEXT, count INTEGER NOT NULL, "
                     "score REAL NOT NULL, PRIMARY KEY(context,word)) WITHOUT ROWID;")
    ngrams: collections.Counter[tuple[str, str]] = collections.Counter()
    with sentence_path.open(encoding="utf-8") as source:
        for row_number, line in enumerate(source, 1):
            sentence = line.partition("\t")[2]
            words = [match.group().lower() for match in TOKEN.finditer(sentence)]
            for index in range(1, len(words)):
                ngrams[(words[index - 1], words[index])] += 1
            for index in range(2, len(words)):
                ngrams[(words[index - 2] + " " + words[index - 1], words[index])] += 1
            if row_number % 5000 == 0:
                db.executemany("INSERT INTO n VALUES(?,?,?,0) ON CONFLICT DO UPDATE SET "
                               "count=count+excluded.count", ((c, w, n) for (c, w), n in ngrams.items()))
                db.commit(); ngrams.clear()
    if ngrams:
        db.executemany("INSERT INTO n VALUES(?,?,?,0) ON CONFLICT DO UPDATE SET "
                       "count=count+excluded.count", ((c, w, n) for (c, w), n in ngrams.items()))
        db.commit()

    pack = reset_pack("phrase")
    with (pack / "context-scores.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for context, word, count, significance in db.execute(
                "SELECT context,word,count,score FROM n ORDER BY context,word"):
            score = max(float(significance), 10.0 * math.log10(count + 1))
            output.write(f"{context}\t{word}\t{score:.6f}\n")
    db.close(); database.unlink(missing_ok=True)
    return finish_pack("phrase", "Phrase / N-gram", "eng-news-2025-1M",
                       "ranking-phrase-context", "context-scores.tsv",
                       "TEKITO_PHRASE_INDEX_V1", 2, "CC-BY-4.0",
                       "https://downloads.wortschatz-leipzig.de/corpora/eng_news_2025_1M.tar.gz",
                       leipzig_notice("Adjacent bigram and trigram data"), "2025-1M")


def build_wordnet() -> dict:
    base = CACHE / "wordnet" / "WordNet-3.0"
    license_text = require(base / "LICENSE").read_text(encoding="latin-1")
    pos_names = {"noun": "noun", "verb": "verb", "adj": "adjective", "adv": "adverb"}
    entries: dict[str, list[str]] = {}
    for suffix, pos_name in pos_names.items():
        with require(base / "dict" / f"data.{suffix}").open(encoding="latin-1") as source:
            for line in source:
                if not line[:1].isdigit() or " | " not in line:
                    continue
                data, gloss = line.split(" | ", 1)
                fields = data.split()
                try:
                    word_count = int(fields[3], 16)
                except (ValueError, IndexError):
                    continue
                words = [fields[4 + index * 2].replace("_", " ") for index in range(word_count)]
                definition = clean(gloss.split('; "', 1)[0])
                examples = re.findall(r'"([^"]+)"', gloss)
                example = clean(examples[0]) if examples else ""
                for word in words:
                    if not ASCII_TERM.fullmatch(word):
                        continue
                    key = "external:" + word.lower()
                    if key not in entries:
                        entries[key] = [word, pos_name, definition, example]
                    elif definition and definition not in entries[key][2]:
                        entries[key][2] = clean(entries[key][2] + " / " + definition)
    pack = reset_pack("dictionary-display")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for key in sorted(entries):
            headword, pos, definition, example = entries[key]
            output.write(f"{key}\t{headword}\t\t{pos}\t{definition}\t{example}\n")
    return finish_pack("dictionary-display", "Extended Dictionary", "wordnet-3.0",
                       "dictionary-display", "entries.tsv", "TEKITO_DICTIONARY_INDEX_V1", 1,
                       "WordNet-3.0", "https://wordnet.princeton.edu/download/current-version",
                       "Dictionary definitions and examples are derived from Princeton WordNet 3.0.\n\n" +
                       license_text, "3.0")


def build_pronunciation() -> dict:
    rows: dict[str, set[str]] = collections.defaultdict(set)
    with require(CACHE / "cmudict.dict").open(encoding="utf-8") as source:
        for line in source:
            word, separator, pronunciation = line.rstrip("\n").partition(" ")
            word = re.sub(r"\(\d+\)$", "", word).lower()
            if separator and ASCII_WORD.fullmatch(word):
                rows[word].add(clean(pronunciation))
    pack = reset_pack("pronunciation")
    with (pack / "pronunciations.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for word in sorted(rows):
            output.write(f"{word}\t{' | '.join(sorted(rows[word]))}\n")
    license_text = require(CACHE / "cmudict-LICENSE").read_text(encoding="utf-8")
    return finish_pack("pronunciation", "Pronunciation Data", "cmudict-2025-10-24",
                       "pronunciation", "pronunciations.tsv", "TEKITO_PRONUNCIATION_INDEX_V1", 1,
                       "CMUdict", "https://github.com/cmusphinx/cmudict", license_text,
                       "master-2025-10-24")


def read_emoji_test_sequences(path: Path) -> dict[str, str]:
    """Return normalized emoji-test keys and their fully-qualified spelling."""
    sequences: dict[str, str] = {}
    for line in require(path).read_text(encoding="utf-8").splitlines():
        if ";" not in line or line.lstrip().startswith("#"):
            continue
        codepoints, status_and_name = line.split(";", 1)
        status = status_and_name.split("#", 1)[0].strip()
        if status not in {"fully-qualified", "minimally-qualified", "unqualified"}:
            continue
        sequence = "".join(chr(int(value, 16)) for value in codepoints.split())
        normalized = sequence.replace("\ufe0f", "")
        if status == "fully-qualified" or normalized not in sequences:
            sequences[normalized] = sequence
    if not sequences:
        raise ValueError(f"emoji test data is empty: {path}")
    return sequences


def build_cldr() -> dict:
    rows: set[tuple[str, str]] = set()
    base = CACHE / "cldr" / "common"
    emoji_sequences = read_emoji_test_sequences(CACHE / "emoji-test-17.0.0.txt")
    for path in (require(base / "annotations" / "en.xml"),
                 require(base / "annotationsDerived" / "en.xml")):
        for annotation in ET.parse(path).iter("annotation"):
            emoji = annotation.attrib.get("cp", "").replace("\ufe0f", "")
            canonical_emoji = emoji_sequences.get(emoji)
            if annotation.attrib.get("type") == "tts" or canonical_emoji is None:
                continue
            for keyword in (annotation.text or "").split("|"):
                keyword = clean(keyword).lower()
                if ASCII_TERM.fullmatch(keyword):
                    rows.add((keyword, canonical_emoji))
    pack = reset_pack("emoji")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for keyword, emoji in sorted(rows):
            output.write(f"{keyword}\t{emoji}\temoji\ten-US\tCLDR 48.2 annotation\n")
    license_text = require(CACHE / "unicode-LICENSE").read_text(encoding="utf-8")
    return finish_pack("emoji", "Emoji Data", "48.2+emoji-17.0.0", "emoji-annotations", "entries.tsv",
                       "TEKITO_SLANG_INDEX_V1", 1, "Unicode-3.0",
                       "https://unicode.org/Public/cldr/48.2/cldr-common-48.2.zip; "
                       "https://www.unicode.org/Public/17.0.0/emoji/emoji-test.txt",
                       "Emoji annotations are derived from Unicode CLDR 48.2 and filtered "
                       "to sequences listed by Unicode Emoji 17.0.0 emoji-test.txt. "
                       "Variation selector U+FE0F is normalized for CLDR matching and the "
                       "fully-qualified Emoji spelling is emitted.\n\n" +
                       license_text, "CLDR 48.2 + Emoji 17.0.0")


def build_geonames() -> dict:
    archive = require(CACHE / "geonames-allCountries.zip")
    database = CACHE / "proper-nouns-build.sqlite3"
    database.unlink(missing_ok=True)
    db = sqlite3.connect(database)
    db.executescript("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=FILE;"
                     "CREATE TABLE names(key TEXT PRIMARY KEY, display TEXT NOT NULL, population INTEGER NOT NULL) WITHOUT ROWID;")
    batch: list[tuple[str, str, int]] = []
    with zipfile.ZipFile(archive) as zipped:
        member = next(name for name in zipped.namelist() if name.endswith("allCountries.txt"))
        with zipped.open(member) as raw:
            for byte_line in raw:
                fields = byte_line.decode("utf-8", "replace").rstrip("\n").split("\t")
                if len(fields) < 15:
                    continue
                population = int(fields[14] or 0)
                for name in (fields[1], fields[2], *fields[3].split(",")):
                    name = clean(name)
                    if ASCII_TERM.fullmatch(name):
                        batch.append((name.lower(), name, population))
                if len(batch) >= 50000:
                    db.executemany("INSERT INTO names VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET "
                                   "display=CASE WHEN excluded.population>population THEN excluded.display ELSE display END,"
                                   "population=max(population,excluded.population)", batch)
                    db.commit(); batch.clear()
    if batch:
        db.executemany("INSERT INTO names VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET "
                       "display=CASE WHEN excluded.population>population THEN excluded.display ELSE display END,"
                       "population=max(population,excluded.population)", batch)
        db.commit()
    pack = reset_pack("proper-nouns")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for key, display in db.execute("SELECT key,display FROM names ORDER BY key"):
            output.write(f"{key}\t{display}\tproper\ten-US\tGeoNames place name\n")
    db.close(); database.unlink(missing_ok=True)
    notice = require(CACHE / "geonames-readme.txt").read_text(encoding="utf-8", errors="replace")
    return finish_pack("proper-nouns", "Extended Proper Nouns", "2026-08-12",
                       "proper-noun-gazetteer", "entries.tsv", "TEKITO_SLANG_INDEX_V1", 1,
                       "CC-BY-4.0", "https://download.geonames.org/export/dump/allCountries.zip",
                       "Place names derived from GeoNames allCountries dump.\n\n" + notice,
                       "2026-08-12")


def build_wiktionary_slang() -> dict:
    source_path = require(CACHE / "raw-wiktextract-data.jsonl.gz")
    database = CACHE / "slang-build.sqlite3"
    database.unlink(missing_ok=True)
    db = sqlite3.connect(database)
    db.executescript("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=FILE;"
                     "CREATE TABLE slang(raw TEXT, candidate TEXT, label TEXT, PRIMARY KEY(raw,candidate,label)) WITHOUT ROWID;")
    wanted = {"slang", "colloquial", "internet", "texting", "abbreviation", "initialism", "acronym"}
    wanted_bytes = tuple(tag.encode() for tag in wanted)
    batch: list[tuple[str, str, str]] = []
    with gzip.open(source_path, "rb") as source:
        for raw_line in source:
            if not any(tag in raw_line for tag in wanted_bytes) and b'"pos": "abbrev"' not in raw_line:
                continue
            try:
                entry = json.loads(raw_line)
            except json.JSONDecodeError:
                continue
            if entry.get("lang_code") != "en":
                continue
            word = clean(entry.get("word", ""))
            if not ASCII_TERM.fullmatch(word):
                continue
            tags = {str(tag).lower() for tag in entry.get("tags", [])}
            for sense in entry.get("senses", []):
                tags.update(str(tag).lower() for tag in sense.get("tags", []))
            relevant = tags & wanted
            if not relevant and entry.get("pos") != "abbrev":
                continue
            label = "abbreviation" if entry.get("pos") == "abbrev" or relevant & {"abbreviation", "initialism", "acronym"} else "slang"
            batch.append((word.lower(), word, label))
            if len(batch) >= 25000:
                db.executemany("INSERT OR IGNORE INTO slang VALUES(?,?,?)", batch)
                db.commit(); batch.clear()
    if batch:
        db.executemany("INSERT OR IGNORE INTO slang VALUES(?,?,?)", batch)
        db.commit()
    pack = reset_pack("wiktionary-slang")
    with (pack / "entries.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for raw, candidate, label in db.execute("SELECT raw,candidate,label FROM slang ORDER BY raw,candidate,label"):
            output.write(f"{raw}\t{candidate}\t{label}\ten-US\tWiktionary label\n")
    db.close(); database.unlink(missing_ok=True)
    return finish_pack("wiktionary-slang", "Extended Slang and Abbreviations", "enwiktionary-2026-08-05",
                       "slang-abbreviation", "entries.tsv", "TEKITO_SLANG_INDEX_V1", 1,
                       "CC-BY-SA-4.0", "https://kaikki.org/dictionary/rawdata.html",
                       "English slang and abbreviation labels are derived from the English Wiktionary "
                       "dump dated 2026-08-05 using Wiktextract. Copyright belongs to Wiktionary "
                       "contributors. This derived pack is distributed under CC BY-SA 4.0.\n"
                       "Source histories: https://en.wiktionary.org/ and https://dumps.wikimedia.org/enwiktionary/\n"
                       "License: https://creativecommons.org/licenses/by-sa/4.0/\n"
                       "TEKITO selected English entries explicitly tagged slang, colloquial, internet, "
                       "texting, abbreviation, initialism, or acronym; normalized lookup keys; sorted; and indexed them.",
                       "dump-2026-08-05")


def print_summary() -> None:
    """Prints every pack under data/ with its size and license."""
    rows = []
    for manifest_path in sorted(DATA.glob("*/manifest.json")):
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        data_path = manifest_path.parent / manifest["file"]
        index_path = manifest_path.parent / manifest["index_file"]
        size = data_path.stat().st_size + index_path.stat().st_size
        rows.append((manifest["pack_id"], manifest.get("entry_count"), size,
                     manifest["version"], manifest["license"]))
    print(f"{'Pack':32} {'Entries':>12} {'Size':>10}  Version / License")
    for pack_id, count, size, version, license_id in rows:
        entries = f"{count:,}" if count is not None else "-"
        print(f"{pack_id:32} {entries:>12} {size / 2 ** 20:>8.1f}MB  {version} / {license_id}")
    total = sum(row[2] for row in rows)
    print(f"{len(rows)} packs, {total / 2 ** 30:.2f} GB")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", choices=("wikipedia", "frequency", "phrase", "wordnet",
                                            "pronunciation", "emoji", "proper-nouns", "slang"))
    parser.add_argument("--complete-standard", action="store_true")
    args = parser.parse_args()
    only = args.only
    if args.complete_standard and only is None:
        complete_standard_english()
        print_summary()
        return 0
    manifests = []
    if only in (None, "wikipedia"):
        manifests.append(build_wikipedia())
    if only in (None, "frequency", "phrase"):
        words_path, co_path, sentence_path = leipzig_paths()
        by_id, counts = read_leipzig_words(words_path)
        if only in (None, "frequency"):
            manifests.append(build_frequency(by_id, counts))
        if only in (None, "phrase"):
            manifests.append(build_phrase(sentence_path))
    if only in (None, "wordnet"):
        manifests.append(build_wordnet())
    if only in (None, "pronunciation"):
        manifests.append(build_pronunciation())
    if only in (None, "emoji"):
        manifests.append(build_cldr())
    if only in (None, "proper-nouns"):
        manifests.append(build_geonames())
    if only in (None, "slang"):
        manifests.append(build_wiktionary_slang())
    for manifest in manifests:
        print(f"Built {manifest['display_name']}: {manifest['entry_count']:,} entries")
    print_summary()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Data Pack build failed: {error}", file=sys.stderr)
        raise
