#!/usr/bin/env python3
"""Validate TEKITO data-pack structure, indexes, hashes, and lexical coverage."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
from datapacks import DATA, manifests, pack_dir  # noqa: E402
WORD = re.compile(r"^[A-Za-z]+(?:['-][A-Za-z]+)*$")

EXPECTED_CORRECTIONS = {
    ("fontrier", "frontier"),
    ("mediciney", "medicine"),
    ("manouverability", "manoeuvrability"),
    ("phonecian", "phoenician"),
    ("vigeur", "vigour"),
}
AUDITED_STANDARD_ADDITIONS = {
    "anthropomorphization", "archimedean", "cellpadding", "compatibilities",
    "correctors", "differentiations", "disputandum", "drumless", "endoliths",
    "extremophile", "futhark", "futhorc", "geometers", "hydrophile",
    "hydrophobe", "interpretor", "johannine", "mccarthyist", "milieux",
    "millennialism", "octahedra", "papanicolaou", "parallelly", "premillennial",
    "premonstratensians", "prolegomena", "repartition", "resignment",
    "unmaneuverable", "unmanoeuvrable",
}
AMBIGUOUS_SOURCE_WORDS = {
    "causalities", "discontentment", "florescent", "indite", "knifes", "loosing",
    "lsat", "midwifes", "nickle", "payed", "planed", "slippy", "specif", "thru",
    "toke", "tuscon", "withing",
}
REJECTED_CORRECTION_TARGETS = {"fontier", "mediciny", "manoeuverability",
                               "phoenecian", "vigueur", "unitedstates"}
REJECTED_CORRECTION_PAIRS = {
    ("muhammadan", "muslim"),  # semantic replacement, not spelling correction
}

EXPECTED_FIELDS = {
    "standard-english": 1,
    "wikipedia-common-misspellings": 2,
    "frequency": 2,
    "phrase": 3,
    "dictionary-display": 6,
    "pronunciation": 2,
    "proper-nouns": 5,
    "slang": 5,
    "wiktionary-slang": 5,
    "emoji": 5,
    "social-expression": 6,
    "japanese-phonetic": 5,
    "japanese-romaji": 3,
    "japanese-loanwords": 3,
}
UNIQUE_KEY_PACKS = {
    "standard-english", "frequency", "japanese-romaji",
    "dictionary-display", "pronunciation", "proper-nouns",
}
INDEX_HEADERS = {
    "standard-english": "TEKITO_LEXICON_INDEX_V1",
    "wikipedia-common-misspellings": "TEKITO_WIKIPEDIA_MISSPELLING_INDEX_V1",
    "frequency": "TEKITO_FREQUENCY_INDEX_V1",
    "phrase": "TEKITO_PHRASE_INDEX_V1",
    "dictionary-display": "TEKITO_DICTIONARY_INDEX_V1",
    "pronunciation": "TEKITO_PRONUNCIATION_INDEX_V1",
    "qwerty-typo-catalog": "TEKITO_INPUT_CATALOG_INDEX_V1",
    "japanese-romaji": "TEKITO_JAPANESE_ROMAJI_INDEX_V1",
}
# Packs whose rows may start with "#" (the romaji table maps the # key).
NO_COMMENT_PACKS = {"japanese-romaji"}
INDEX_KEY_FIELDS = {pack_id: 2 if pack_id == "phrase" else 1
                    for pack_id in EXPECTED_FIELDS}
SOCIAL_KINDS = {"text_expansion", "emoticon", "kaomoji", "symbol", "emoji"}
SOCIAL_RANGES = {
    "text_expansion": "common",
    "emoticon": "familiar",
    "kaomoji": "familiar",
    "symbol": "broad",
    "emoji": "broad",
}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest().upper()


def text_lines(path: Path):
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8", newline="") as source:
        for number, line in enumerate(source, 1):
            yield number, line.rstrip("\r\n")


def relative_to_pack(pack: Path, value: str) -> Path:
    path = (pack / value).resolve()
    try:
        path.relative_to(pack.resolve())
    except ValueError as error:
        raise ValueError(f"path escapes pack: {value}") from error
    return path


def validate_index(pack_id: str, data_path: Path, index_path: Path,
                   errors: list[str]) -> None:
    expected_header = INDEX_HEADERS.get(pack_id)
    expected_fields = INDEX_KEY_FIELDS.get(pack_id, 1)
    raw_lines = index_path.read_bytes().splitlines()
    if not raw_lines:
        errors.append(f"{pack_id}: empty index")
        return
    header = raw_lines[0].decode("ascii", errors="replace")
    if expected_header and header != expected_header:
        errors.append(f"{pack_id}: unexpected index header {header!r}")

    previous_offset = -1
    previous_key = b""
    data_size = data_path.stat().st_size
    with data_path.open("rb") as data:
        for number, raw_line in enumerate(raw_lines[1:], 2):
            parts = raw_line.split(b"\t", 1)
            if len(parts) != 2:
                errors.append(f"{pack_id}:{index_path.name}:{number}: malformed index row")
                continue
            try:
                offset = int(parts[0])
            except ValueError:
                errors.append(f"{pack_id}:{index_path.name}:{number}: invalid offset")
                continue
            key = parts[1]
            if offset <= previous_offset or key < previous_key:
                errors.append(f"{pack_id}:{index_path.name}:{number}: index is not sorted")
            if offset < 0 or offset >= data_size:
                errors.append(f"{pack_id}:{index_path.name}:{number}: offset outside data file")
            elif data_path.suffix != ".gz" and pack_id != "qwerty-typo-catalog":
                data.seek(offset)
                data_line = data.readline().rstrip(b"\r\n")
                actual_key = b"\t".join(data_line.split(b"\t", expected_fields)[:expected_fields])
                if actual_key != key:
                    errors.append(
                        f"{pack_id}:{index_path.name}:{number}: key does not match data offset")
            previous_offset, previous_key = offset, key


def validate_sorted_tsv(pack: Path, manifest: dict) -> list[str]:
    """Meaning packs (scripts/ja/build-japanese-meaning-packs.py): one TSV,
    surface, reading, senses..., searched in place, so the rows must be in
    bytewise order of the surface (code point order is the same)."""
    pack_id = pack.name
    errors: list[str] = []
    for field in ("pack_id", "version", "language", "type", "license", "file",
                  "notice_file", "entry_count", "sha256"):
        if field not in manifest:
            errors.append(f"{pack_id}: manifest missing {field}")
    if errors:
        return errors
    try:
        data_path = relative_to_pack(pack, manifest["file"])
        notice_path = relative_to_pack(pack, manifest["notice_file"])
    except ValueError as error:
        return [f"{pack_id}: {error}"]
    for path in (data_path, notice_path):
        if not path.is_file():
            errors.append(f"{pack_id}: missing {path.name}")
    if errors:
        return errors
    if digest(data_path) != manifest["sha256"].get("file"):
        errors.append(f"{pack_id}: data checksum mismatch")
    count = 0
    previous = None
    # Meaning packs: surface, reading, senses. Others say how many fields
    # their rows have (japanese-zipcode: code, address).
    fields_expected = manifest.get("fields")
    for number, line in text_lines(data_path):
        count += 1
        fields = line.split("\t")
        if fields_expected is not None:
            if len(fields) != fields_expected or not all(fields):
                errors.append(f"{pack_id}:{number}: expected {fields_expected} fields")
                continue
        elif len(fields) < 3 or not fields[0] or not all(fields[2:]):
            errors.append(f"{pack_id}:{number}: expected a surface, a reading and senses")
            continue
        key = (fields[0], fields[1])
        if previous is not None and key <= previous:
            errors.append(f"{pack_id}:{number}: rows are not sorted and unique")
        previous = key
    if count != manifest["entry_count"]:
        errors.append(f"{pack_id}: manifest count {manifest['entry_count']} != {count}")
    return errors


def validate_rules(pack: Path, manifest: dict) -> list[str]:
    """The special-conversions pack (scripts/build-special-conversions.py):
    one TSV read whole, section, language, key and value on each row."""
    pack_id = pack.name
    errors = [f"{pack_id}: manifest missing {field}"
              for field in ("pack_id", "version", "language", "type", "license", "file",
                            "notice_file", "entry_count", "sha256") if field not in manifest]
    if errors:
        return errors
    data_path = relative_to_pack(pack, manifest["file"])
    if not data_path.is_file() or not relative_to_pack(pack, manifest["notice_file"]).is_file():
        return [f"{pack_id}: missing files"]
    if digest(data_path) != manifest["sha256"].get("file"):
        errors.append(f"{pack_id}: data checksum mismatch")
    sections = {"word", "format", "name", "era", "symbol", "emoticon", "kanji"}
    count = 0
    for number, line in text_lines(data_path):
        if not line or line.startswith("#"):
            continue
        count += 1
        fields = line.split("\t")
        if len(fields) != 4 or not all(fields):
            errors.append(f"{pack_id}:{number}: expected section, language, key and value")
        elif fields[0] not in sections or fields[1] not in {"en", "ja"}:
            errors.append(f"{pack_id}:{number}: unknown section or language")
    if count != manifest["entry_count"]:
        errors.append(f"{pack_id}: manifest count {manifest['entry_count']} != {count}")
    return errors


def validate_language_model(pack: Path, manifest: dict) -> list[str]:
    """The japanese-lm pack (scripts/ja/build-japanese-lm.py): one binary file,
    mapped and checked by its reader; here it must be what the manifest
    says."""
    pack_id = pack.name
    errors = [f"{pack_id}: manifest missing {field}" for field in ("file", "notice_file", "sha256", "license")
              if field not in manifest]
    if errors:
        return errors
    data_path = relative_to_pack(pack, manifest["file"])
    if not data_path.is_file() or not relative_to_pack(pack, manifest["notice_file"]).is_file():
        return [f"{pack_id}: missing files"]
    if data_path.read_bytes()[:4] != b"TKLM":
        errors.append(f"{pack_id}: {data_path.name} is not a ja-lm file")
    if digest(data_path) != manifest["sha256"].get("file"):
        errors.append(f"{pack_id}: data checksum mismatch")
    return errors


def validate_pack(pack: Path, manifest: dict) -> list[str]:
    if manifest.get("format") == "sorted-tsv-v1":
        return validate_sorted_tsv(pack, manifest)
    if manifest.get("format") == "rules-tsv-v1":
        return validate_rules(pack, manifest)
    if str(manifest.get("format", "")).startswith("ja-lm-"):
        return validate_language_model(pack, manifest)
    errors: list[str] = []
    pack_id = pack.name
    for field in ("pack_id", "version", "language", "type", "license", "file",
                  "index_file", "notice_file", "entry_count", "sha256"):
        if field not in manifest:
            errors.append(f"{pack_id}: manifest missing {field}")
    if errors:
        return errors
    aliases = {"slang": {"slang", "tekito-slang"}}
    if manifest["pack_id"] != pack_id and manifest["pack_id"] not in aliases.get(pack_id, set()):
        errors.append(f"{pack_id}: manifest pack_id mismatch")
    try:
        data_path = relative_to_pack(pack, manifest["file"])
        index_path = relative_to_pack(pack, manifest["index_file"])
        notice_path = relative_to_pack(pack, manifest["notice_file"])
    except ValueError as error:
        return [f"{pack_id}: {error}"]
    for path in (data_path, index_path, notice_path):
        if not path.is_file():
            errors.append(f"{pack_id}: missing {path.name}")
    if errors:
        return errors
    if digest(data_path) != manifest["sha256"].get("file"):
        errors.append(f"{pack_id}: data checksum mismatch")
    if digest(index_path) != manifest["sha256"].get("index"):
        errors.append(f"{pack_id}: index checksum mismatch")
    if not isinstance(manifest["entry_count"], int) or manifest["entry_count"] <= 0:
        errors.append(f"{pack_id}: entry_count must be a positive integer")
    if manifest.get("format") == "ja-dict-v1+ja-matrix-v1":
        # Binary, memory-mapped files (scripts/ja/build-japanese-packs.py); the
        # reader checks every offset, so here the files only need to be what
        # the manifest says.
        if data_path.read_bytes()[:4] != b"TKJD":
            errors.append(f"{pack_id}: dictionary.bin is not a ja-dict file")
        if index_path.read_bytes()[:4] != b"TKJM":
            errors.append(f"{pack_id}: connection.bin is not a ja-matrix file")
        return errors

    expected_fields = EXPECTED_FIELDS.get(pack_id)
    count = 0
    previous_key = None
    seen_keys = set()
    for number, line in text_lines(data_path):
        if not line or (line.startswith("#") and pack_id not in NO_COMMENT_PACKS):
            continue
        count += 1
        if pack_id == "qwerty-typo-catalog":
            try:
                json.loads(line)
            except json.JSONDecodeError:
                errors.append(f"{pack_id}:{number}: invalid JSON")
            continue
        fields = line.split("\t")
        if pack_id == "slang" and fields[0] == "raw":
            count -= 1
            continue
        if expected_fields is not None and len(fields) != expected_fields:
            errors.append(f"{pack_id}:{number}: expected {expected_fields} fields, got {len(fields)}")
            continue
        if not fields[0]:
            errors.append(f"{pack_id}:{number}: empty key")
        key_fields = 2 if pack_id == "phrase" else 1
        key = tuple(fields[:key_fields])
        if pack_id in UNIQUE_KEY_PACKS and key in seen_keys:
            errors.append(f"{pack_id}:{number}: duplicate key {key!r}")
        seen_keys.add(key)
        if previous_key is not None and key < previous_key:
            errors.append(f"{pack_id}:{number}: data is not sorted")
        previous_key = key
        if pack_id == "standard-english" and not WORD.fullmatch(fields[0]):
            errors.append(f"{pack_id}:{number}: invalid word {fields[0]!r}")
        if pack_id == "standard-english" and fields[0] != fields[0].lower():
            errors.append(f"{pack_id}:{number}: word is not lowercase {fields[0]!r}")
        if pack_id == "wikipedia-common-misspellings":
            if not WORD.fullmatch(fields[0]) or not WORD.fullmatch(fields[1]):
                errors.append(f"{pack_id}:{number}: invalid correction row")
        if pack_id == "dictionary-display":
            if not fields[1] or not fields[3] or not fields[4]:
                errors.append(f"{pack_id}:{number}: incomplete dictionary entry")
        if pack_id == "phrase" and (not fields[0] or not fields[1] or not fields[2]):
            errors.append(f"{pack_id}:{number}: incomplete phrase row")
        if pack_id in {"frequency", "phrase"}:
            score_field = 1 if pack_id == "frequency" else 2
            try:
                score = float(fields[score_field])
                if not math.isfinite(score) or score < 0:
                    errors.append(f"{pack_id}:{number}: invalid score {fields[score_field]!r}")
            except ValueError:
                errors.append(f"{pack_id}:{number}: invalid score {fields[score_field]!r}")
        if pack_id in {"slang", "wiktionary-slang", "emoji", "social-expression",
                       "japanese-phonetic", "proper-nouns"} and not fields[1]:
            errors.append(f"{pack_id}:{number}: empty candidate")
    if count != manifest["entry_count"]:
        errors.append(f"{pack_id}: manifest count {manifest['entry_count']} != {count}")

    validate_index(pack_id, data_path, index_path, errors)
    return errors


def validate_social_expression() -> list[str]:
    path = pack_dir("social-expression", DATA) / "entries.tsv"
    errors: list[str] = []
    seen = set()
    for number, line in text_lines(path):
        fields = line.split("\t")
        if len(fields) != 6 or not fields[0] or not fields[1]:
            continue
        raw, candidate, label, _, tier, notes = fields
        metadata = {}
        for part in notes.split(";"):
            if "=" in part:
                key, value = part.split("=", 1)
                metadata[key] = value
        kind = metadata.get("kind", "")
        if kind not in SOCIAL_KINDS:
            errors.append(f"social-expression:{number}: unknown candidate kind {kind!r}")
        elif tier != SOCIAL_RANGES[kind]:
            errors.append(f"social-expression:{number}: kind/range mismatch")
        expected_label = "emoji" if kind == "emoji" else "social"
        if label != expected_label:
            errors.append(f"social-expression:{number}: kind/label mismatch")
        for key in ("policy", "raw_pinned", "auto_apply"):
            if metadata.get(key) not in {
                    "suggest_only" if key == "policy" else "true" if key == "raw_pinned" else "false"}:
                errors.append(f"social-expression:{number}: unsafe policy metadata {key}")
        if any(ord(character) < 0x20 or ord(character) == 0x7F
               for character in raw + candidate + notes):
            errors.append(f"social-expression:{number}: control character in row")
        if kind in {"emoticon", "kaomoji"} and len(candidate) > 64:
            errors.append(f"social-expression:{number}: face expression exceeds 64 code points")
        key = (raw, candidate, label, tier)
        if key in seen:
            errors.append(f"social-expression:{number}: duplicate candidate row")
        seen.add(key)
    return errors


def read_emoji_test_sequences(path: Path) -> set[str]:
    sequences = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if ";" not in line or line.lstrip().startswith("#"):
            continue
        codepoints, status_and_name = line.split(";", 1)
        status = status_and_name.split("#", 1)[0].strip()
        if status in {"fully-qualified", "minimally-qualified", "unqualified"}:
            sequences.add("".join(chr(int(value, 16)) for value in codepoints.split())
                            .replace("\ufe0f", ""))
    return sequences


def validate_emoji_candidates(emoji_test_path: Path | None) -> list[str]:
    if emoji_test_path is None or not emoji_test_path.is_file():
        return []
    sequences = read_emoji_test_sequences(emoji_test_path)
    errors = []
    for number, line in text_lines(pack_dir("emoji", DATA) / "entries.tsv"):
        fields = line.split("\t")
        if len(fields) == 5 and fields[1].replace("\ufe0f", "") not in sequences:
            errors.append(f"emoji:{number}: candidate is absent from emoji-test data")
    return errors


def coverage_report() -> list[str]:
    def keys(path: Path, field: int = 0) -> set[str]:
        result = set()
        for _, line in text_lines(path):
            if line and not line.startswith("#"):
                fields = line.split("\t")
                if len(fields) > field:
                    result.add(fields[field].lower())
        return result

    standard = keys(pack_dir("standard-english", DATA) / "lexicon.txt")
    corroborated = (
        keys(pack_dir("frequency", DATA) / "word-scores.tsv") |
        keys(pack_dir("pronunciation", DATA) / "pronunciations.tsv") |
        keys(pack_dir("dictionary-display", DATA) / "entries.tsv", 1)
    )
    corrections = {
        line.split("\t")[1].lower()
        for _, line in text_lines(pack_dir("wikipedia-common-misspellings", DATA) / "misspellings.tsv")
        if len(line.split("\t")) == 2
    }
    missing = sorted(corrections - standard)
    supported = sorted(set(missing) & corroborated)
    unsupported = sorted(set(missing) - corroborated)
    return [
        f"correction targets missing from standard lexicon: {len(missing)}",
        f"independently corroborated: {len(supported)}",
        f"needs manual review: {len(unsupported)}",
        "manual-review sample: " + ", ".join(unsupported[:20]),
    ]


def validate_corrections() -> list[str]:
    path = pack_dir("wikipedia-common-misspellings", DATA) / "misspellings.tsv"
    pairs = set()
    duplicate_pairs = set()
    for _, line in text_lines(path):
        fields = line.split("\t")
        if len(fields) == 2:
            pair = (fields[0].lower(), fields[1].lower())
            if pair in pairs:
                duplicate_pairs.add(pair)
            pairs.add(pair)
    errors = [f"missing audited correction: {raw} -> {target}"
              for raw, target in EXPECTED_CORRECTIONS if (raw, target) not in pairs]
    errors.extend(f"rejected correction target still present: {target}"
                  for target in REJECTED_CORRECTION_TARGETS
                  if any(candidate == target for _, candidate in pairs))
    errors.extend(f"rejected semantic correction still present: {raw} -> {target}"
                  for raw, target in sorted(REJECTED_CORRECTION_PAIRS)
                  if (raw, target) in pairs)
    errors.extend(f"duplicate normalized correction: {raw} -> {target}"
                  for raw, target in sorted(duplicate_pairs))

    standard = {word.strip().lower()
                for _, word in text_lines(pack_dir("standard-english", DATA) / "lexicon.txt")
                if word.strip()}
    manifest = json.loads((pack_dir("standard-english", DATA) / "manifest.json").read_text(encoding="utf-8"))
    manifest_terms = set(manifest.get("supplemental_manual_review_terms", []))
    if manifest.get("supplemental_manual_review_count") != len(AUDITED_STANDARD_ADDITIONS):
        errors.append("standard-english: supplemental review count is stale")
    if manifest_terms != AUDITED_STANDARD_ADDITIONS:
        errors.append("standard-english: supplemental review term manifest is stale")
    errors.extend(f"audited target missing from standard lexicon: {target}"
                  for target in sorted(AUDITED_STANDARD_ADDITIONS)
                  if target not in standard)
    raw_words = {raw for raw, _ in pairs}
    unexpected_ambiguous = (raw_words & standard) - AMBIGUOUS_SOURCE_WORDS
    errors.extend(f"unreviewed valid word used as misspelling key: {raw}"
                  for raw in sorted(unexpected_ambiguous))
    errors.extend(f"self-correction is not a misspelling: {raw} -> {target}"
                  for raw, target in sorted(pairs) if raw == target)
    return errors


def validate_cross_pack_coverage() -> list[str]:
    def keys(path: Path, field: int = 0) -> set[str]:
        result = set()
        for _, line in text_lines(path):
            if line and not line.startswith("#"):
                fields = line.split("\t")
                if len(fields) > field:
                    result.add(fields[field].lower())
        return result

    standard = keys(pack_dir("standard-english", DATA) / "lexicon.txt")
    frequency = keys(pack_dir("frequency", DATA) / "word-scores.tsv")
    return [f"standard words missing from frequency pack: {word}"
            for word in sorted(standard - frequency)]


def main() -> int:
    global DATA
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", type=Path, default=DATA)
    parser.add_argument("--emoji-test", type=Path,
                        default=ROOT / ".cache" / "tekito-data" / "emoji-test-17.0.0.txt")
    args = parser.parse_args()
    DATA = args.data_root.resolve()
    errors: list[str] = []
    for manifest_path in manifests(DATA):
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        errors.extend(validate_pack(manifest_path.parent, manifest))
    errors.extend(validate_corrections())
    errors.extend(validate_cross_pack_coverage())
    errors.extend(validate_social_expression())
    errors.extend(validate_emoji_candidates(args.emoji_test.resolve()))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"validated {len(manifests(DATA))} data packs")
    print("\n".join(coverage_report()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
