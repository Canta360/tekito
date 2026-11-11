# Third-Party Notices

TEKITO includes or builds with the following third-party components. The
corresponding source notices are kept under `third_party/` in this repository,
and each data pack's under `data/<pack>/`. This file is a release inventory,
not a replacement for the original license texts.

## SQLite and SQLite3 Multiple Ciphers

TEKITO keeps its user data with SQLite (public domain), as built by SQLite3
Multiple Ciphers 2.5.1 (Copyright 2019-2026 Ulrich Telle, MIT License,
`third_party/sqlite3mc/LICENSE`), which adds encryption of the database
when the user turns it on. See `third_party/sqlite3mc/README.md`.

## Monocypher

Monocypher 4.0.2 (https://monocypher.org/) derives the key from the recovery
code of an encrypted database and wraps the database's key with it. It is
dedicated to the public domain under CC0 1.0, or licensed under the 2-clause
BSD license, at your choice (`third_party/monocypher/LICENCE.md`).

## Microsoft WebView2

The Settings window uses the Microsoft WebView2 SDK. Its redistribution
notice and license files are included under `third_party/webview2/`.
The runtime itself is a Windows prerequisite and is not bundled by the
TEKITO package.

## React

The Settings page bundles React, React DOM and Scheduler (Copyright Meta
Platforms, Inc. and affiliates), licensed under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.

Vite and its React plugin are used only to build the page and are not
shipped.

## Material Symbols

The icons in Settings are Material Symbols (Rounded, weight 300; Copyright
Google LLC), from the `@material-symbols/svg-300` package, licensed under the
Apache License 2.0. The full license text is
`third_party/material-symbols/LICENSE` (shipped in the package at the same
path, and in Settings → About → Third-party notices).

## M PLUS 1

M PLUS 1 (Copyright 2021 The M+ FONTS Project Authors,
https://github.com/coz-m/MPLUS_FONTS) is the TEKITO typeface. It is embedded,
unmodified, in the setup wizard, in `Tekito.Tsf.dll` for the candidate window,
and in the Settings UI. It is licensed under the SIL Open Font License 1.1; the
full license text is `assets/fonts/OFL.txt` (shipped in the package at the same
path).

## Cat sounds

The sounds of the Cat option are "Meow" by IgnasD
(https://opengameart.org/content/meow) and "Kitten Mew" by AntumDeluge
(https://opengameart.org/content/kitten-mew), both dedicated to the public
domain under CC0 1.0, with only the silence before and after them removed.
They are embedded in `Tekito.Tsf.dll`.

## Language data

The data packs in `data/` (and in the installer, or the Japanese data it
downloads) come from:

- SCOWL, Wikipedia, the Leipzig Corpora Collection, WordNet, GeoNames,
  Wiktionary, the CMU Pronouncing Dictionary and Unicode CLDR;
- the Mozc OSS dictionary and its symbol, emoji, emoticon and single-kanji
  tables (IPAdic, BSD 3-Clause), KANJIDIC2, JMdict and JMnedict (EDRDG),
  Wikidata (CC0), SudachiDict (Works Applications, Apache-2.0, with parts of
  UniDic and NEologd), Japan Post's postal code data (for place names),
  Unicode Unihan, the SKK dictionaries' itaiji list (public domain) and the
  Japanese WordNet (NICT);
- data written for TEKITO.

Each pack's `NOTICE` and `manifest.json` give its source, version and
license, and carry the attribution that license asks for; they are the
authoritative notices for the data. [docs/data-packs.md](docs/data-packs.md)
lists them together.

## The sentence check

The model that checks whole Japanese sentences (the japanese-reranker pack)
was trained on the zenz-v2.5 dataset (llm-jp-corpus-v3 part, Open Data Commons
Attribution License 1.0), ABEJA-CC-JA (PDDL 1.0), YODAS's Japanese captions
(CC BY 3.0), ASDC (Megagon Labs, CC BY 4.0), the Rosebleu dataset
(Apache-2.0), Tatoeba (CC BY 2.0 FR), Common Voice's text sentences (CC0) and
the Leipzig Corpora Collection's Japanese news. It holds no text from them.

## The optional AI models

Downloaded only when the user chooses AI:

- the models: Liquid AI's LFM2.5, fine-tuned by TEKITO, under the LFM Open
  License v1.0 (each model pack's `LICENSE` and `NOTICE`);
- what runs them: ONNX Runtime (Microsoft, MIT License) and the Microsoft
  Visual C++ runtime DLLs (Visual Studio license terms), with their notices
  in the runtime pack.

Microsoft's Windows ML (onnxruntime.dll, DirectML.dll and
Microsoft.Windows.AI.MachineLearning.dll, under the Microsoft Software License
Terms for the Windows ML Runtime, `third_party/windows-ml/LICENSE.txt`) is
downloaded only when the user chooses to run the models on a GPU or an NPU
and agrees to those terms.

## Postal codes

Japan Post's postal code data is not included: Settings downloads it (from
the postal-codes release, a monthly copy of Japan Post's file, which Japan
Post allows to be redistributed, or from Japan Post), or reads a file the
user downloaded, only when the user turns it on.
