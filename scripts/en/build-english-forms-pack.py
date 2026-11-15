"""Builds the english-forms Data Pack: English word forms the meaning pane
cannot find as they are, with the word they come from and what form they
are (went: go, past tense; children: child, plural).

Regular forms (called, players, running, bigger) are worked out from their
endings by the meaning pane itself; this pack has the irregular ones.

  forms.tsv  form <TAB> base <TAB> kind, sorted bytewise by form; a form
             may have rows for more than one base. A one-word lemma also
             has a row with no base and its parts of speech as the kind
             (call <TAB> <TAB> parts:noun verb), for the endings' forms.

Kinds: past, past-participle, past+past-participle, present-participle,
third-person, plural, comparative, superlative, inflection (a form of the
word whose kind the source does not say).

Sources: WordNet 3.0's exception lists (verb.exc, noun.exc, adj.exc,
adv.exc; https://wordnet.princeton.edu/, the WordNet 3.0 license), and
scripts/en/irregular-verbs.tsv (written for TEKITO), which tells a verb's
past tense from its past participle.

  python scripts/en/build-english-forms-pack.py --wordnet <WordNet-3.0/dict>
"""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION = "2026.11"


def exceptions(path: Path) -> list[tuple[str, list[str]]]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) >= 2:
            rows.append((fields[0].replace("_", " "), [base.replace("_", " ") for base in fields[1:]]))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wordnet", type=Path, required=True, help="WordNet 3.0's dict folder")
    args = parser.parse_args()

    spec = importlib.util.spec_from_file_location("full", ROOT / "scripts" / "en" / "build-full-data-packs.py")
    full = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(full)

    rows: dict[tuple[str, str], str] = {}

    def add(form: str, base: str, kind: str) -> None:
        if form == base or not base or not form.isascii() or not base.isascii():
            return
        key = (form, base)
        # A known kind wins over "inflection"; past and past participle join.
        before = rows.get(key)
        if before is None or before == "inflection":
            rows[key] = kind
        elif {before, kind} == {"past", "past-participle"}:
            rows[key] = "past+past-participle"

    # The irregular verbs written for TEKITO: past tense and past participle.
    for line in (ROOT / "scripts" / "en" / "irregular-verbs.tsv").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        base, past, participle = line.split("\t")
        for form in past.split("/"):
            add(form, base, "past")
        for form in participle.split("/"):
            add(form, base, "past-participle")

    for form, bases in exceptions(args.wordnet / "verb.exc"):
        for base in bases:
            kind = ("present-participle" if form.endswith("ing")
                    else "past+past-participle" if form.endswith("ed")
                    else "third-person" if form.endswith("s") and not base.endswith("s")
                    else "inflection")
            add(form, base, kind)
    for form, bases in exceptions(args.wordnet / "noun.exc"):
        for base in bases:
            add(form, base, "plural")
    # Comparative and superlative: by ending, else the one of a word's two
    # forms that ends in -st is the superlative (worse, worst).
    for name in ("adj.exc", "adv.exc"):
        graded: dict[str, list[str]] = {}
        for form, bases in exceptions(args.wordnet / name):
            for base in bases:
                graded.setdefault(base, []).append(form)
        for base, forms in graded.items():
            for form in forms:
                if form.endswith("er"):
                    kind = "comparative"
                elif form.endswith("st"):
                    kind = "superlative"
                elif any(other.endswith("st") for other in forms if other != form):
                    kind = "comparative"
                else:
                    kind = "inflection"
                add(form, base, kind)

    # Each one-word lemma's parts of speech.
    parts: dict[str, list[str]] = {}
    for name, part in (("index.noun", "noun"), ("index.verb", "verb"), ("index.adj", "adj"), ("index.adv", "adv")):
        for line in (args.wordnet / name).read_text(encoding="utf-8").splitlines():
            if not line or line.startswith(" "):
                continue
            lemma = line.split(" ", 1)[0]
            if "_" in lemma or not lemma.isascii() or not lemma.isalpha():
                continue
            parts.setdefault(lemma, [])
            if part not in parts[lemma]:
                parts[lemma].append(part)
    for lemma, names in parts.items():
        rows[(lemma, "")] = "parts:" + " ".join(names)

    pack = full.reset_pack("english-forms")
    with (pack / "forms.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for (form, base), kind in sorted(rows.items(), key=lambda item: (item[0][0].encode("utf-8"), item[0][1])):
            output.write(f"{form}\t{base}\t{kind}\n")
    manifest = full.finish_pack(
        "english-forms", "English Word Forms", VERSION, "english-forms", "forms.tsv",
        "TEKITO_SLANG_INDEX_V1", 1, "WordNet-3.0 + TEKITO-OWNED",
        "WordNet 3.0 exception lists (https://wordnet.princeton.edu/) and scripts/en/irregular-verbs.tsv",
        "Irregular English word forms and the words they come from, for the meaning pane: from WordNet 3.0's "
        "exception lists (verb.exc, noun.exc, adj.exc, adv.exc), and a list of common irregular verbs written "
        "for TEKITO that tells a past tense from a past participle.\n\n"
        "WordNet 3.0 Copyright 2006 by Princeton University. All rights reserved.\n\n"
        + (args.wordnet.parent / "LICENSE").read_text(encoding="utf-8").strip(),
        source_version="3.0")
    print(f"Built {manifest['display_name']}: {manifest['entry_count']:,} forms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
