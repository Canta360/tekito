# Installer

`package-release.ps1` turns a Release build into an offline package and a
single-file installer:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
.\installer\package-release.ps1 -ValidateOnly   # check the inputs
.\installer\package-release.ps1
```

It writes to `artifacts\`:

- `TEKITO-<version>-full\`, the package: `TEKITO.exe`, `Tekito.Tsf.dll`, the
  Settings page, the Data Packs, the install scripts and the license texts,
  with `package-manifest.json` holding the SHA-256 of every file.
- `TEKITO-<version>-full.zip`, the same package zipped.
- `TEKITO-<version>-full-installer.exe`, the setup program with the ZIP
  appended.

The setup program (`TekitoSetup.cpp`) checks for the WebView2 Runtime, shows
the license agreement, then unpacks the ZIP to a temporary folder, runs
`verify-package.ps1` and `install.ps1`. To look at its screens without
installing anything, build the `tekito_setup_preview` target.

The scripts can also be run by hand from an administrator PowerShell in the
extracted package:

| Script | What it does |
| --- | --- |
| `verify-package.ps1` | Checks every file against the package manifest. |
| `install.ps1` | Installs to `%LOCALAPPDATA%\Programs\TEKITO`, installs the Data Packs to `%LOCALAPPDATA%\TEKITO\data`, registers the input method and adds a Start menu shortcut. |
| `uninstall.ps1` | Removes the program. `-RemoveUserData` also deletes the dictionary, learning, settings and Data Packs. |
| `scripts\verify-installed.ps1` | Read-only check of an installation. |

The package and installer are not signed yet, so SmartScreen and Smart App
Control may block them.
