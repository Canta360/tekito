#!/usr/bin/env python3
"""Builds the spelling-variants pack: American and British spellings of the
same word (color, colour), for the English spelling setting.

The source is VarCon (Kevin Atkinson, Benjamin Titze; http://wordlist.aspell.net/),
varcon.txt from the v1 branch of github.com/en-wl/wordlist. Only lines that
name one preferred American (A) and one preferred British (B) spelling, both
plain lower-case words, are kept; British -ise is used where VarCon gives it
(B), not the Oxford -ize (Z). A spelling British English also uses for some
sense (check: a bank's cheque, but check to verify) is left out; senses
VarCon marks as rare, "(-)", do not count.

Output: data/en/spelling-variants/variants.tsv, "american<TAB>british", sorted.

Usage: python scripts/build-spelling-variants.py [varcon dir]
(default: .cache/tekito-data/varcon of the main checkout)
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT.parents[2] if ROOT.parent.name == "worktrees" else ROOT
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else MAIN / ".cache" / "tekito-data" / "varcon"
OUT = ROOT / "data" / "en" / "spelling-variants"
WORD = re.compile(r"[a-z][a-z'-]*")


def parse(line: str) -> dict[str, str]:
    """The word for each tag (A, B...) on a VarCon line."""
    line = line.split("|", 1)[0].strip()
    words: dict[str, str] = {}
    for part in line.split(" / "):
        if ":" not in part:
            continue
        tags, word = part.split(":", 1)
        word = word.strip()
        for tag in tags.split():
            words.setdefault(tag, word)
    return words


def main() -> int:
    pairs: dict[str, str] = {}
    british_of: dict[str, set[str]] = {}
    shared: set[str] = set()
    for line in (SOURCE / "varcon.txt").read_text(encoding="latin-1").splitlines():
        if not line or line.startswith("#"):
            continue
        words = parse(line)
        note = line.split("|", 1)[1] if "|" in line else ""
        if words.get("A") and words.get("A") == words.get("B") and "(-)" not in note:
            shared.add(words["A"])
        american, british = words.get("A"), words.get("B")
        if not american or not british or american == british:
            continue
        if not WORD.fullmatch(american) or not WORD.fullmatch(british):
            continue
        pairs.setdefault(american, british)
        british_of.setdefault(british, set()).add(american)
    # A British spelling of two American words (practise: practice, practise)
    # is left out: which is meant is not known.
    rows = sorted((a, b) for a, b in pairs.items()
                  if len(british_of[b]) == 1 and b not in pairs and a not in shared and b not in shared)
    OUT.mkdir(parents=True, exist_ok=True)
    data = OUT / "variants.tsv"
    data.write_text("".join(f"{a}\t{b}\n" for a, b in rows), encoding="utf-8", newline="\n")
    license_text = (SOURCE / "varcon-README.txt").read_text(encoding="latin-1")
    license_text = license_text[license_text.index("Copyright\n=========") :]
    (OUT / "NOTICE").write_text(
        "American and British spellings from VarCon (Variant Conversion Info),\n"
        "http://wordlist.aspell.net/, made into one table by\n"
        "scripts/build-spelling-variants.py.\n\n" + license_text.strip() + "\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "spelling-variants",
        "display_name": "American and British Spellings",
        "schema_version": 1,
        "version": "varcon-2020.12.07",
        "language": "en-US+en-GB",
        "type": "spelling-variants",
        "file": "variants.tsv",
        "entry_count": len(rows),
        "sha256": {"file": hashlib.sha256(data.read_bytes()).hexdigest().upper()},
        "license": "VarCon (permissive, notice required)",
        "source": "https://github.com/en-wl/wordlist/tree/v1/varcon",
        "notice_file": "NOTICE",
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows):,} pairs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
