# TEKITO Privacy Notice

Version: 0.1.0 prerelease

## Design summary

TEKITO input processing, candidate generation, ranking, and dictionary lookup
are designed to run locally from installed Data Packs. The application does
not intentionally send typed text, input history, User Dictionary entries, or
User Learning data to a TEKITO server. TEKITO has no account or telemetry
service in this package.

The WebView2-based Settings UI is local content. The installer does not
download TEKITO data during normal installation. If WebView2 Runtime is
missing, the installer can open the official Microsoft download page only
after the user chooses that action.

## Local data locations

The following locations may contain user or machine-specific data:

| Location | Contents | User-provided data possible |
| --- | --- | --- |
| `%LOCALAPPDATA%\TEKITO\data` | Installed Data Packs | No, unless the user adds a pack |
| `%LOCALAPPDATA%\TEKITO\user.db` | User Dictionary, User Learning, and settings | Yes |
| `%LOCALAPPDATA%\TEKITO\settings-webview` | WebView2 profile, cache, and local UI state | May contain settings/UI state |
| `%TEMP%\TekitoTsf.log` | Optional diagnostic trace | Designed to exclude typed text and input history |

The current diagnostic design excludes typed input and input history from
diagnostics, but users should still treat local logs and the SQLite database
as private files. TEKITO does not claim that the SQLite database is encrypted.

## User controls and removal

Normal uninstall preserves User Dictionary, User Learning, and Settings. The
explicit `uninstall.ps1 -RemoveUserData` path removes the local TEKITO User Data
and installed Data Packs. Users should back up any User Dictionary they want to
keep before using that option.

Windows, Microsoft Edge WebView2, the browser used to obtain a prerequisite,
and any host application may have separate diagnostic, privacy, and retention
behavior. Those services are outside TEKITO's control and are governed by
their own notices.

## Japanese summary

TEKITOの入力処理・候補生成・辞書検索はローカルData Packを使って動作し、
TEKITOのサーバーへ入力文字列、入力履歴、User Dictionary、学習データを
意図的に送信しません。このパッケージにはアカウント機能やテレメトリ機能は
ありません。

User Dictionary・学習・設定は `%LOCALAPPDATA%\TEKITO\user.db`、WebView2の
ローカル状態は `settings-webview`、診断ログは通常 `%TEMP%` に保存されます。
通常のアンインストールではUser Dataを保持し、完全削除は
`uninstall.ps1 -RemoveUserData`を明示的に実行した場合だけ行います。
SQLiteの暗号化は現状保証していません。
