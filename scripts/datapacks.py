"""Where the Data Packs are in the repository.

The repository keeps them by language, so English and Japanese can be
worked on apart: data/en, data/ja, and data/common for the ones both use.
TEKITO installs them side by side in one folder, and finds them either way
(FindDataPack in src/Core/DataPackPath.h). The scripts under scripts/en and
scripts/ja import this module after putting scripts/ on the path.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"

# The folder each pack belongs in. A pack missing here is a mistake.
LANGUAGE = {
    "dictionary-display": "en",
    "emoji": "en",
    "frequency": "en",
    "japanese-phonetic": "en",  # romaji spellings of English words, for English input
    "phrase": "en",
    "pronunciation": "en",
    "proper-nouns": "en",
    "qwerty-typo-catalog": "en",
    "slang": "en",
    "social-expression": "en",
    "standard-english": "en",
    "wikipedia-common-misspellings": "en",
    "wiktionary-slang": "en",
    "japanese-core": "ja",
    "japanese-english": "ja",
    "japanese-lm": "ja",
    "japanese-loanwords": "ja",
    "japanese-romaji": "ja",
    "japanese-slang": "ja",
    "japanese-wiktionary": "ja",
    "japanese-wordnet": "ja",
    "japanese-zipcode": "ja",
    "special-conversions": "common",
}


def pack_dir(pack_id: str, data: Path = DATA) -> Path:
    """The folder of `pack_id` under `data`: in its language folder, or side
    by side as TEKITO installs them when only that exists. A pack not built
    yet goes in its language folder."""
    grouped = data / LANGUAGE[pack_id] / pack_id
    installed = data / pack_id
    return installed if not grouped.is_dir() and installed.is_dir() else grouped


def manifests(data: Path = DATA) -> list[Path]:
    """Every pack's manifest under `data`, by language folder or side by side."""
    return sorted(set(data.glob("*/manifest.json")) | set(data.glob("*/*/manifest.json")))
