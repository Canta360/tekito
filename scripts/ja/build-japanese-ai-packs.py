#!/usr/bin/env python3
"""Writes the optional AI Data Packs: the models (japanese-ai-light,
japanese-ai-strong) and what runs them (ai-runtime: TekitoAI.exe and
llama.cpp), and a zip of each for Settings and the installer to download.

  build-japanese-ai-packs.py [--light DIR] [--strong DIR]
                             [--runtime DIR --llama-license FILE] --out DIR

--light, --strong: a train-japanese-ai-model.py run's folder (model-Q4_K_M.gguf
for --light, model-Q8_0.gguf for --strong, made from its model with
llama.cpp's convert_hf_to_gguf.py --outtype f16 and llama-quantize) with the
base model's LICENSE copied into it (LFM2.5-350M-Base for --light,
LFM2.5-1.2B-JP-202606 for --strong). Writes <out>/<pack>/ and <out>/TEKITO-<pack>.zip,
and prints each zip's SHA-256 and size for apps/tekito-settings.
--runtime: the build's Release folder (TekitoAI.exe and the llama.cpp DLLs
next to it); --llama-license: llama.cpp's LICENSE; --crt: the Visual C++
runtime's redistributable folder.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

PACKS = {
    "japanese-ai-light": {
        "display_name": "Japanese AI model (light)",
        "file": "model-Q4_K_M.gguf",
        "source": "LiquidAI/LFM2.5-350M-Base (https://huggingface.co/LiquidAI/LFM2.5-350M-Base), "
                  "fine-tuned by TEKITO (scripts/ja/train-japanese-ai-model.py)",
        # Trained for conversion: reads the reading and writes the text.
        "manifest": {"prompt": "tekito-reading-v1", "margin": 2.0},
        "notice": """Based on LFM2.5-350M-Base by Liquid AI
(https://huggingface.co/LiquidAI/LFM2.5-350M-Base), under the LFM Open
License v1.0 (LICENSE). Modified by TEKITO: fine-tuned to convert
readings into Japanese text, converted to GGUF with llama.cpp and
quantized to Q4_K_M.

Fine-tuned on:
- the zenz-v2.5 dataset's llm-jp-corpus-v3 part
  (https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset), under
  the Open Data Commons Attribution License (ODC-BY 1.0);
- English sentences from the Leipzig Corpora Collection
  (eng_news_2025_1M, https://wortschatz.uni-leipzig.de/), under CC BY 4.0;
- Japanese and English mixed, made from those Japanese lines with their
  katakana words put in English: loanwords from the CMU Pronouncing
  Dictionary (BSD-style licence) ranked by the Leipzig frequencies, and
  names from Wikidata's labels (https://www.wikidata.org/, CC0 1.0).
""",
    },
    "japanese-ai-strong": {
        "display_name": "Japanese AI model (strong)",
        "file": "model-Q8_0.gguf",
        "source": "LiquidAI/LFM2.5-1.2B-JP-202606 (https://huggingface.co/LiquidAI/LFM2.5-1.2B-JP-202606), "
                  "fine-tuned by TEKITO (scripts/ja/train-japanese-ai-model.py, LoRA)",
        "manifest": {"prompt": "tekito-reading-v1", "margin": 2.0},
        "notice": """Based on LFM2.5-1.2B-JP-202606 by Liquid AI
(https://huggingface.co/LiquidAI/LFM2.5-1.2B-JP-202606), under the LFM Open
License v1.0 (LICENSE). Modified by TEKITO: fine-tuned with low-rank
adapters (merged in) to convert readings into Japanese text, converted to
GGUF with llama.cpp and quantized to Q8_0.

Fine-tuned on:
- the zenz-v2.5 dataset's llm-jp-corpus-v3 part
  (https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset), under
  the Open Data Commons Attribution License (ODC-BY 1.0);
- English sentences from the Leipzig Corpora Collection
  (eng_news_2025_1M, https://wortschatz.uni-leipzig.de/), under CC BY 4.0.
""",
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def build(pack: str, source: Path, out: Path, version: str) -> None:
    spec = PACKS[pack]
    directory = out / pack
    directory.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / spec["file"], directory / "model.gguf")
    shutil.copyfile(source / "LICENSE", directory / "LICENSE")
    (directory / "NOTICE").write_text(spec["notice"], encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": pack,
        "display_name": spec["display_name"],
        "schema_version": 1,
        "version": version,
        "language": "ja-JP",
        "type": "japanese-ai-model",
        "format": "gguf",
        "file": "model.gguf",
        "sha256": {"file": sha256(directory / "model.gguf")},
        "license": "LFM Open License v1.0",
        "license_file": "LICENSE",
        "source": spec["source"],
        "notice_file": "NOTICE",
    }
    manifest.update(spec.get("manifest", {}))
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    archive = out / f"TEKITO-{pack}.zip"
    # Stored, not deflated: the weights do not compress, and Windows' tar
    # unpacks a stored zip quickly.
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED) as z:
        for name in ("manifest.json", "model.gguf", "LICENSE", "NOTICE"):
            z.write(directory / name, f"{pack}/{name}")
    print(f"{pack}: {archive.name} sha256={sha256(archive)} bytes={archive.stat().st_size}")


RUNTIME_NOTICE = """TekitoAI.exe is TEKITO's (see TEKITO's license). It is built with
llama.cpp and ggml (https://github.com/ggml-org/llama.cpp), under the MIT
License (LICENSE), whose DLLs come with it, and with the Microsoft Visual
C++ runtime DLLs (msvcp140, vcruntime140), redistributed under the Visual
Studio license terms.
"""


CRT = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]


def build_runtime(release: Path, llama_license: Path, crt: Path, out: Path, version: str) -> None:
    pack = "ai-runtime"
    directory = out / pack
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    names = ["TekitoAI.exe", "llama.dll", "ggml.dll", "ggml-base.dll"]
    names += sorted(p.name for p in release.glob("ggml-cpu-*.dll"))
    names += [p.name for p in release.glob("ggml-vulkan.dll")]
    for name in names:
        shutil.copyfile(release / name, directory / name)
    # The llama.cpp DLLs use the Visual C++ runtime's DLLs, which not every
    # PC has: they go next to them.
    for name in CRT:
        shutil.copyfile(crt / name, directory / name)
    names += CRT
    shutil.copyfile(llama_license, directory / "LICENSE")
    (directory / "NOTICE").write_text(RUNTIME_NOTICE, encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": pack,
        "display_name": "AI runtime",
        "schema_version": 1,
        "version": version,
        "language": "mul",
        "type": "ai-runtime",
        "protocol": "TKA2",
        "file": "TekitoAI.exe",
        "sha256": {"file": sha256(directory / "TekitoAI.exe")},
        "files": names,
        "license": "MIT (llama.cpp)",
        "license_file": "LICENSE",
        "source": "https://github.com/ggml-org/llama.cpp",
        "notice_file": "NOTICE",
    }
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    archive = out / f"TEKITO-{pack}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for name in ["manifest.json", "LICENSE", "NOTICE"] + names:
            z.write(directory / name, f"{pack}/{name}")
    print(f"{pack}: {archive.name} sha256={sha256(archive)} bytes={archive.stat().st_size}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--light")
    parser.add_argument("--strong")
    parser.add_argument("--runtime")
    parser.add_argument("--llama-license")
    parser.add_argument("--crt", help="VC/Redist/MSVC/<version>/x64/Microsoft.VC*.CRT in Visual Studio")
    parser.add_argument("--out", required=True)
    parser.add_argument("--version", default="2026.10")
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.light:
        build("japanese-ai-light", Path(args.light), out, args.version)
    if args.strong:
        build("japanese-ai-strong", Path(args.strong), out, args.version)
    if args.runtime:
        if not args.llama_license or not args.crt:
            parser.error("--runtime needs --llama-license and --crt")
        build_runtime(Path(args.runtime), Path(args.llama_license), Path(args.crt), out, args.version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
