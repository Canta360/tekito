# Third-Party Notices

TEKITO includes or builds with the following third-party components. The
corresponding source notices are kept under `third_party/` in this repository.

## SQLite

The SQLite amalgamation is distributed under the SQLite public-domain terms.
See `third_party/sqlite/README.md` and the notices in the amalgamation source.

## Microsoft WebView2

The Settings application uses the Microsoft WebView2 SDK. Its redistribution
notice and license files are included under `third_party/webview2/`.
The runtime itself is a Windows prerequisite and is not bundled by the
TEKITO package.

## Settings UI dependencies

The Settings UI is built from the packages declared in
`apps/tekito-settings/ui/package.json`. Their package metadata and license
files are resolved during the UI build; the release package contains only the
compiled UI assets. Before publishing a release, review the dependency tree
and include any required notices for the selected release version.

## M PLUS 1

M PLUS 1 (Copyright 2021 The M+ FONTS Project Authors,
https://github.com/coz-m/MPLUS_FONTS) is the TEKITO typeface. It is embedded,
unmodified, in the setup wizard, in `Tekito.Tsf.dll` for the candidate window,
and in the Settings UI. It is licensed under the SIL Open Font License 1.1; the
full license text is `assets/fonts/OFL.txt` (shipped in the package at the same
path).

This file is a release inventory, not a replacement for the original license
texts.

## TEKITO Data Packs

Data Packs are distributed independently from the TEKITO source and binaries.
The exact source, version, checksum, license, and attribution for each shipped
pack are recorded in `data/<pack-id>/manifest.json` and
`data/<pack-id>/NOTICE`. Those per-pack notices are the authoritative release
notices for Data Pack redistribution.
