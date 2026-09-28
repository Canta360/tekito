# Privacy

TEKITO works entirely on your PC. It does not send what you type, your
dictionary or what it has learned anywhere. It has no account, no telemetry
and no update check, and nothing in the input path uses the network.

## What TEKITO keeps, and where

| Location | What is in it |
| --- | --- |
| `%LOCALAPPDATA%\TEKITO\user.db` | Your settings, your dictionary, and what TEKITO has learned from your choices |
| `%LOCALAPPDATA%\TEKITO\data` | The language data packs |
| `%LOCALAPPDATA%\TEKITO\settings-webview` | The Settings window's browser cache |
| `%TEMP%\TekitoTsf.log` | A diagnostic log, written only by trace builds or when `TEKITO_TSF_TRACE=1` is set |

What TEKITO learns is a count of which suggestions you pick or turn down for
a word, and in Japanese, which conversion you pick for a reading. It does not
store sentences or a history of what you typed. To read a sentence as a
whole, Japanese conversion keeps the last few words you committed in memory
only, while you go on typing right after them; they are dropped once the
caret moves elsewhere and are never saved. The
diagnostic log and **Copy diagnostics** in Settings record events, timings
and error codes, never text.

The database is not encrypted; treat it like any other private file in your
profile.

## Your controls

- **Settings → Dictionary & learning → Learn from my choices** turns learning
  on or off, and **Forget** clears what TEKITO has learned. Japanese
  conversions can be forgotten on their own there.
- **Settings → General → Apps where TEKITO stays off** makes TEKITO pass
  every key through in the apps you list. It also stays off in terminals,
  apps running as administrator, and password, PIN, email, URL and number
  fields.
- Uninstalling keeps your dictionary, learning and settings. Run
  `uninstall.ps1 -RemoveUserData` from the installer package to remove them
  too.

## Other software

Windows, the Microsoft Edge WebView2 Runtime that draws the Settings window,
and the apps you type in have their own privacy terms. If the WebView2
Runtime is missing, the setup program offers to open Microsoft's download
page; it never downloads anything on its own.

## The Japanese data download

When you choose Japanese in the setup program, it downloads the Japanese
data from TEKITO's release on GitHub (github.com) and checks it against the
checksum it carries. That request is the only network use; it sends nothing
about you beyond what any download does, such as your IP address, and
GitHub's privacy terms apply to it. Installing from a ZIP you downloaded
yourself (`install.ps1 -JapaneseDataPath`) makes no request at all.

---

## プライバシー（日本語）

TEKITO の処理はすべて PC の中で行われます。入力した内容、ユーザー辞書、学習した内容をどこにも送りません。アカウント、テレメトリ、更新確認はなく、入力の処理経路でネットワークを使うこともありません。

- 設定・ユーザー辞書・学習内容は `%LOCALAPPDATA%\TEKITO\user.db` に保存されます。学習しているのは「どの単語でどの候補を選んだか」と、日本語では「どの読みにどの変換を選んだか」の回数だけで、文章や入力履歴は保存しません。日本語の変換は文全体を読むために直前に確定した数語をメモリーにだけ持ちますが、続けて打っている間だけ使い、カーソルがほかへ移ると忘れます。保存はしません。
- インストール時に日本語を選ぶと、日本語データを GitHub の TEKITO のリリースからダウンロードし、チェックサムを確かめます。ネットワークを使うのはこのときだけです。自分でダウンロードした ZIP から入れる場合（`install.ps1 -JapaneseDataPath`）は通信しません。
- 診断ログ（`%TEMP%\TekitoTsf.log`、トレース用ビルドか `TEKITO_TSF_TRACE=1` のときだけ作られます）と設定画面の「Copy diagnostics」には、イベント・時間・エラーコードだけが含まれ、入力した文字は含まれません。
- 学習は設定画面の「辞書と学習 → 選んだ候補から学ぶ」でオン/オフでき、「消去」で消せます。日本語の変換の学習だけを消すこともできます。「一般 → TEKITO をオフにするアプリ」に追加したアプリでは完全にオフになります。
- アンインストールしても辞書・学習・設定は残ります。消す場合はインストーラーのパッケージにある `uninstall.ps1 -RemoveUserData` を実行してください。
- データベースは暗号化していません。ほかの個人ファイルと同じように扱ってください。
