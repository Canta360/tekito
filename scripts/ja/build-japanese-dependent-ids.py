"""Adds dependent-ids.txt to a built japanese-core pack: the parts of speech
(Mozc's ids, from its id.def, as the pack) of words that only follow other
words: suffixes and dependent nouns (名詞,接尾 and 名詞,非自立: 化, 下, 家 as
in 近代化, 年下, 作家). TEKITO makes a word of one kana with such a part of
speech, and none other for the same written word, cost more at the start of
what is typed (かに: 蟹, not 化に; 日 and 目 are nouns too and stay).

One id per line.

Usage:
  python scripts/ja/build-japanese-dependent-ids.py --mozc <Mozc src/data> --pack <japanese-core pack>
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

FILE = "dependent-ids.txt"
DEPENDENT = ("名詞,接尾,", "名詞,非自立,")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mozc", type=Path, required=True, help="Mozc's src/data (dictionary_oss/id.def in it)")
    parser.add_argument("--pack", type=Path, required=True)
    args = parser.parse_args()

    id_def = args.mozc / "dictionary_oss" / "id.def"
    if not id_def.is_file():
        id_def = args.mozc / "id.def"
    ids = []
    for line in id_def.read_text(encoding="utf-8").splitlines():
        number, _, pos = line.partition(" ")
        if pos.startswith(DEPENDENT):
            ids.append(int(number))
    path = args.pack / FILE
    path.write_text("".join(f"{i}\n" for i in sorted(ids)), encoding="utf-8", newline="\n")
    manifest_path = args.pack / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    files = manifest.get("files") or [manifest["file"]]
    if FILE not in files:
        files.append(FILE)
    manifest["files"] = files
    manifest.setdefault("sha256", {})["dependent-ids"] = hashlib.sha256(path.read_bytes()).hexdigest().upper()
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(ids)} dependent parts of speech in {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
