# Third-Party Notices

TEKITO includes or builds with the following third-party components. The
corresponding source notices are kept under `third_party/` in this repository.

## SQLite

The SQLite amalgamation is distributed under the SQLite public-domain terms.
See `third_party/sqlite/README.md` and the notices in the amalgamation source.

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

## M PLUS 1

M PLUS 1 (Copyright 2021 The M+ FONTS Project Authors,
https://github.com/coz-m/MPLUS_FONTS) is the TEKITO typeface. It is embedded,
unmodified, in the setup wizard, in `Tekito.Tsf.dll` for the candidate window,
and in the Settings UI. It is licensed under the SIL Open Font License 1.1; the
full license text is `assets/fonts/OFL.txt` (shipped in the package at the same
path).

This file is a release inventory, not a replacement for the original license
texts.

## Language data

The data packs in `data/` (and in the installer, or the Japanese data it
downloads) come from SCOWL, Wikipedia, the Leipzig Corpora Collection,
WordNet, GeoNames, Wiktionary, the CMU Pronouncing Dictionary, Unicode CLDR,
the Mozc OSS dictionary (IPAdic, BSD 3-Clause) and the Japanese WordNet
(NICT), plus data written for TEKITO. Each
pack's `NOTICE` and `manifest.json` give its source, version and license, and
carry the attribution that license asks for; they are the authoritative
notices for the data. [docs/data-packs.md](docs/data-packs.md) lists them
together.
