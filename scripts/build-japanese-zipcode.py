"""Builds the japanese-zipcode data pack: the addresses each postal code has.

zipcodes.tsv holds one row per postal code and address, sorted bytewise:
  code <TAB> address
where the code is seven ASCII digits and the address is the prefecture,
city and town ("1000001  東京都千代田区千代田"). TEKITO searches it in place, so
converting 1000001 or 100-0001 in Japanese offers the address.

The source is Japan Post's postal code data in UTF-8 (utf_ken_all.zip from
https://www.post.japanpost.jp/zipcode/dl/utf-zip.html), for which Japan Post
claims no copyright. Japan Post's site does not serve it to scripts, so
download it in a browser first.

Usage:
  python scripts/build-japanese-zipcode.py --ken-all <utf_ken_all.zip>
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import re
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "data" / "japanese-zipcode"

# Town names that are notes, not places: the code covers the rest of the city.
NOT_A_TOWN = re.compile(r"^(以下に掲載がない場合|.*の次に番地がくる場合)$")
# Notes in parentheses after a town ("大通西（１～１９丁目）").
NOTE = re.compile(r"（.*?）|（.*$")


def read_rows(source: Path) -> list[tuple[str, str]]:
    with zipfile.ZipFile(source) as archive:
        name = next(n for n in archive.namelist() if n.lower().endswith(".csv"))
        text = archive.read(name).decode("utf-8-sig")
    rows: set[tuple[str, str]] = set()
    for fields in csv.reader(io.StringIO(text)):
        if len(fields) < 9 or not re.fullmatch(r"\d{7}", fields[2]):
            continue
        town = NOTE.sub("", fields[8]).strip()
        if NOT_A_TOWN.match(town):
            town = ""
        rows.add((fields[2], fields[6] + fields[7] + town))
    return sorted(rows, key=lambda row: (row[0].encode("utf-8"), row[1].encode("utf-8")))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ken-all", type=Path, required=True, help="Japan Post's utf_ken_all.zip")
    args = parser.parse_args()

    rows = read_rows(args.ken_all)
    PACK.mkdir(parents=True, exist_ok=True)
    data = PACK / "zipcodes.tsv"
    with data.open("w", encoding="utf-8", newline="\n") as out:
        for code, address in rows:
            out.write(f"{code}\t{address}\n")
    (PACK / "NOTICE").write_text(
        "Postal codes and the addresses they cover, from Japan Post's postal\n"
        "code data (https://www.post.japanpost.jp/zipcode/download.html).\n"
        "Japan Post claims no copyright in this data and allows it to be\n"
        "redistributed freely. Built by scripts/build-japanese-zipcode.py.\n",
        encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": "japanese-zipcode",
        "display_name": "Japanese Postal Codes",
        "schema_version": 1,
        # The month the data was downloaded, as Japan Post updates it monthly.
        "version": time.strftime("%Y.%m", time.localtime(args.ken_all.stat().st_mtime)),
        "language": "ja-JP",
        "type": "japanese-zipcode",
        "format": "sorted-tsv-v1",
        "fields": 2,
        "file": "zipcodes.tsv",
        "entry_count": len(rows),
        "sha256": {"file": sha256(data)},
        "license": "Japan-Post-Postal-Code-Data (no copyright claimed)",
        "source": "Japan Post utf_ken_all.zip (https://www.post.japanpost.jp/zipcode/dl/utf-zip.html)",
        "notice_file": "NOTICE",
    }
    (PACK / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
                                        encoding="utf-8", newline="\n")
    print(f"japanese-zipcode: {len(rows)} rows, {data.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
