#!/usr/bin/env python3
"""Writes the optional AI Data Packs: the models (japanese-ai-light,
japanese-ai-strong) and what runs them (ai-runtime: TekitoAI.exe with
ONNX Runtime), and a zip of each for Settings and the installer to download.

  build-japanese-ai-packs.py [--light DIR] [--strong DIR]
                             [--runtime DIR --windows-ml DIR --crt DIR] --out DIR

--settings: LFM2.5-350M-Base itself, exported the same way, for Settings'
Home (settings-ai).
--light, --strong: a model exported by export-japanese-ai-onnx.py (--bits 8
for --strong) from a train-japanese-ai-model.py run, with the base model's
LICENSE copied into it (LFM2.5-350M-Base for --light, LFM2.5-1.2B-JP-202606
for --strong). Writes <out>/<pack>/ and <out>/TEKITO-<pack>.zip, and prints
each zip's SHA-256 and size for apps/tekito-settings.

--runtime: the build's Release folder. Two runtime packs come from it, both
ai-runtime: TEKITO-ai-runtime.zip (ai-runtime-cpu/: TekitoAI.exe with
ONNX Runtime's own release, MIT, for the processor), which Settings
downloads with the first model, and TEKITO-ai-runtime-windows-ml.zip
(TekitoAI.exe with Windows ML's ONNX Runtime, DirectML and its provider
catalog, for GPUs and NPUs), which it downloads in its place only once the
user chooses a GPU or an NPU and agrees to Microsoft's terms.
--windows-ml: the Windows ML NuGet package's folder (its license.txt and
ThirdPartyNotices.txt); --crt: the Visual C++ runtime's redistributable
folder.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

# What export-japanese-ai-onnx.py writes for a model pack.
MODEL_FILES = ["model.onnx", "body-1x64.onnx", "body-10x32.onnx", "head-320.onnx", "head.onnx", "model.data",
               "vocab.txt", "merges.txt"]

TRAINING = """Fine-tuned on:
- the zenz-v2.5 dataset's llm-jp-corpus-v3 part
  (https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset), under
  the Open Data Commons Attribution License (ODC-BY 1.0);
- English sentences from the Leipzig Corpora Collection
  (eng_news_2025_1M, https://wortschatz.uni-leipzig.de/), under CC BY 4.0;
- Japanese and English mixed, made from those Japanese lines with their
  katakana words put in English: loanwords from the CMU Pronouncing
  Dictionary (BSD-style licence) ranked by the Leipzig frequencies, and
  names from Wikidata's labels (https://www.wikidata.org/, CC0 1.0).
"""

PACKS = {
    "japanese-ai-light": {
        "display_name": "Japanese AI model (light)",
        "source": "LiquidAI/LFM2.5-350M-Base (https://huggingface.co/LiquidAI/LFM2.5-350M-Base), "
                  "fine-tuned by TEKITO (scripts/ja/train-japanese-ai-model.py)",
        # Trained for conversion: reads the reading and writes the text.
        "manifest": {"prompt": "tekito-reading-v1", "margin": 2.0},
        "notice": """Based on LFM2.5-350M-Base by Liquid AI
(https://huggingface.co/LiquidAI/LFM2.5-350M-Base), under the LFM Open
License v1.0 (LICENSE). Modified by TEKITO: fine-tuned to convert
readings into Japanese text, exported to ONNX and quantized to 4 bits.

""" + TRAINING,
    },
    "japanese-ai-strong": {
        "display_name": "Japanese AI model (strong)",
        "source": "LiquidAI/LFM2.5-1.2B-JP-202606 (https://huggingface.co/LiquidAI/LFM2.5-1.2B-JP-202606), "
                  "fine-tuned by TEKITO (scripts/ja/train-japanese-ai-model.py, LoRA)",
        "manifest": {"prompt": "tekito-reading-v1", "margin": 2.0},
        "notice": """Based on LFM2.5-1.2B-JP-202606 by Liquid AI
(https://huggingface.co/LiquidAI/LFM2.5-1.2B-JP-202606), under the LFM Open
License v1.0 (LICENSE). Modified by TEKITO: fine-tuned with low-rank
adapters (merged in) to convert readings into Japanese text, exported to
ONNX and quantized to 8 bits.

""" + TRAINING,
    },
    # Settings' Home: reads what the user asks for and which setting it
    # means (by how likely each setting's description reads after it).
    "settings-ai": {
        "display_name": "Settings AI model",
        "source": "LiquidAI/LFM2.5-350M-Base (https://huggingface.co/LiquidAI/LFM2.5-350M-Base)",
        "language": "mul",
        "type": "settings-ai-model",
        # Not trained for conversion: reads text alone; scored 10 texts of up
        # to 64 tokens at a time on a GPU or an NPU (exported --shape 10x64).
        "files": ["model.onnx", "body-10x64.onnx", "head-640.onnx", "head.onnx", "model.data", "vocab.txt", "merges.txt"],
        "manifest": {"prompt": "plain", "score_graph": "body-10x64.onnx", "score_head": "head-640.onnx"},
        "notice": """LFM2.5-350M-Base by Liquid AI
(https://huggingface.co/LiquidAI/LFM2.5-350M-Base), under the LFM Open
License v1.0 (LICENSE). Modified by TEKITO: exported to ONNX and quantized
to 4 bits; not trained further.
""",
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def write_zip(out: Path, pack: str, directory: Path, names: list[str], stored: bool) -> None:
    archive = out / f"TEKITO-{directory.name}.zip"
    # Weights stored, not deflated: they do not compress, and Windows' tar
    # unpacks a stored zip quickly.
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED) as z:
        for name in names:
            z.write(directory / name, f"{pack}/{name}")
    print(f"{directory.name}: {archive.name} sha256={sha256(archive)} bytes={archive.stat().st_size}")


def build(pack: str, source: Path, out: Path, version: str) -> None:
    spec = PACKS[pack]
    directory = out / pack
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    files = spec.get("files", MODEL_FILES)
    for name in files:
        shutil.copyfile(source / name, directory / name)
    shutil.copyfile(source / "LICENSE", directory / "LICENSE")
    (directory / "NOTICE").write_text(spec["notice"], encoding="utf-8", newline="\n")
    manifest = {
        "pack_id": pack,
        "display_name": spec["display_name"],
        "schema_version": 1,
        "version": version,
        "language": spec.get("language", "ja-JP"),
        "type": spec.get("type", "japanese-ai-model"),
        "format": "onnx",
        "file": "model.onnx",
        "sha256": {"file": sha256(directory / "model.onnx"), "model.data": sha256(directory / "model.data")},
        "files": files,
        "license": "LFM Open License v1.0",
        "license_file": "LICENSE",
        "source": spec["source"],
        "notice_file": "NOTICE",
    }
    manifest.update(spec.get("manifest", {}))
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    write_zip(out, pack, directory, ["manifest.json"] + files + ["LICENSE", "NOTICE"], stored=True)


CRT = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]
WINDOWS_ML = ["onnxruntime.dll", "DirectML.dll", "Microsoft.Windows.AI.MachineLearning.dll"]

RUNTIME_NOTICE = """TekitoAI.exe is TEKITO's (see TEKITO's license). It runs the AI models
with ONNX Runtime (https://github.com/microsoft/onnxruntime), {runtime}
It comes with the Microsoft Visual C++ runtime DLLs (msvcp140,
vcruntime140), redistributed under the Visual Studio license terms.
"""
CPU_RUNTIME = """as Microsoft
releases it, under the MIT License (LICENSE; its third-party notices in
ThirdPartyNotices.txt)."""
WINDOWS_ML_RUNTIME = """as Microsoft's
Windows ML ships it (onnxruntime.dll, DirectML.dll,
Microsoft.Windows.AI.MachineLearning.dll), under the Microsoft Software
License Terms for the Windows ML Runtime (LICENSE; its third-party notices
in ThirdPartyNotices.txt). These files are Microsoft's, not TEKITO's: TEKITO's
license does not apply to them."""


def build_runtime(release: Path, windows_ml: Path, crt: Path, out: Path, version: str) -> None:
    pack = "ai-runtime"
    variants = [
        ("ai-runtime", release / "ai-runtime-cpu", ["onnxruntime.dll"], release / "ai-runtime-cpu" / "LICENSE",
         release / "ai-runtime-cpu" / "ThirdPartyNotices.txt", "MIT (ONNX Runtime)", CPU_RUNTIME, "cpu"),
        ("ai-runtime-windows-ml", release, WINDOWS_ML, windows_ml / "license.txt", windows_ml / "ThirdPartyNotices.txt",
         "Microsoft Software License Terms (Windows ML Runtime)", WINDOWS_ML_RUNTIME, "windows-ml"),
    ]
    for folder, source, dlls, license_file, notices, license_name, runtime, variant in variants:
        directory = out / folder
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        names = ["TekitoAI.exe"] + dlls
        for name in names:
            shutil.copyfile(source / name, directory / name)
        # TekitoAI.exe and ONNX Runtime use the Visual C++ runtime's DLLs,
        # which not every PC has: they go next to them.
        for name in CRT:
            shutil.copyfile(crt / name, directory / name)
        names += CRT
        shutil.copyfile(license_file, directory / "LICENSE")
        shutil.copyfile(notices, directory / "ThirdPartyNotices.txt")
        (directory / "NOTICE").write_text(RUNTIME_NOTICE.format(runtime=runtime), encoding="utf-8", newline="\n")
        manifest = {
            "pack_id": pack,
            "display_name": "AI runtime",
            "schema_version": 1,
            "version": version,
            "language": "mul",
            "type": "ai-runtime",
            "protocol": "TKA3",
            "variant": variant,
            "file": "TekitoAI.exe",
            "sha256": {"file": sha256(directory / "TekitoAI.exe")},
            "files": names,
            "license": license_name,
            "license_file": "LICENSE",
            "source": "https://github.com/microsoft/onnxruntime",
            "notice_file": "NOTICE",
        }
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
        write_zip(out, pack, directory, ["manifest.json", "LICENSE", "ThirdPartyNotices.txt", "NOTICE"] + names,
                  stored=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--light")
    parser.add_argument("--strong")
    parser.add_argument("--settings", help="LFM2.5-350M-Base exported by export-japanese-ai-onnx.py")
    parser.add_argument("--runtime", help="the build's Release folder")
    parser.add_argument("--windows-ml", help="the Windows ML NuGet package's folder")
    parser.add_argument("--crt", help="VC/Redist/MSVC/<version>/x64/Microsoft.VC*.CRT in Visual Studio")
    parser.add_argument("--out", required=True)
    parser.add_argument("--version", default="2026.11")
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    if args.light:
        build("japanese-ai-light", Path(args.light), out, args.version)
    if args.strong:
        build("japanese-ai-strong", Path(args.strong), out, args.version)
    if args.settings:
        build("settings-ai", Path(args.settings), out, args.version)
    if args.runtime:
        if not args.windows_ml or not args.crt:
            parser.error("--runtime needs --windows-ml and --crt")
        build_runtime(Path(args.runtime), Path(args.windows_ml), Path(args.crt), out, args.version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
