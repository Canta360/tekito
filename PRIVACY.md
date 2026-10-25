# Privacy

TEKITO works entirely on your PC. It does not send what you type, your
dictionary or what it has learned anywhere. It has no account, no telemetry
and no update check, and nothing in the input path uses the network. The
only downloads are the ones you ask for: the Japanese data in the setup
program and the postal code data in Settings.

## What TEKITO keeps, and where

| Location | What is in it |
| --- | --- |
| `%LOCALAPPDATA%\TEKITO\user.db` (and `user.db-wal`, `user.db-shm` beside it) | Your settings, your dictionary, what TEKITO has learned from your choices, and your last 30 Japanese conversions while that is on |
| `%LOCALAPPDATA%\TEKITO\data` | The language data packs |
| `%LOCALAPPDATA%\TEKITO\settings-webview` | The Settings window's browser cache |
| `%TEMP%\TekitoTsf.log` | A diagnostic log, written only by trace builds or when `TEKITO_TSF_TRACE=1` is set |

What TEKITO learns is a count of which suggestions you pick or turn down for
a word, and in Japanese, which conversion you pick for a reading. It does not
store a history of what you typed, except the last 30 Japanese conversions
(the reading and what it became, never the keys pressed and none from a
private window), kept so that you can report a wrong one; Settings → About →
**Report wrong conversions** turns this off and deletes them. To read a sentence as a
whole, Japanese conversion keeps the last few words you committed in memory
only, while you go on typing right after them; they are dropped once the
caret moves elsewhere and are never saved. The text you just entered is also
kept in memory so that Ctrl+Backspace can take it back, until you type on.
Reconversion (Henkan on selected text) reads the selected text on your
computer only, and importing words from another IME reads the file you
choose on your computer only; neither sends anything. The
diagnostic log and **Copy diagnostics** (About) in Settings record events, timings
and error codes, never text.

The database is not encrypted; treat it like any other private file in your
profile.

So that TEKITO also works in the Start menu's search, Settings and Store
apps, the setup program lets those sandboxed apps read the program folder
(`%LOCALAPPDATA%\Programs\TEKITO`) and the language data
(`%LOCALAPPDATA%\TEKITO\data`). They get read access only, and not to
`user.db`: your dictionary and what TEKITO has learned stay out of their
reach, and there TEKITO types with the default settings.

## Your controls

- **Settings → Dictionary & learning → Learn from chosen candidates** turns
  learning on or off, and **Delete all** clears what TEKITO has learned.
- **Settings → About → Report wrong conversions** turns the list of recent
  conversions on or off. **Report** opens a GitHub issue form in your browser
  with the reading and the result filled in; nothing is sent until you submit
  it there. **Report a problem** fills in the version and settings the same
  way.
- **Settings → General → More settings → Apps where TEKITO is off** makes TEKITO pass
  every key through in the apps you list. It also stays off in terminals,
  apps running as administrator, and password, PIN, email, URL and number
  fields.
- Uninstalling from Installed apps removes TEKITO with its data, dictionary,
  learning and settings. It can export your dictionary to Documents first.

## Other software

Windows, the Microsoft Edge WebView2 Runtime that draws the Settings window,
and the apps you type in have their own privacy terms. If the WebView2
Runtime is missing, the setup program offers to open Microsoft's download
page; it never downloads anything on its own.

## The Japanese data download

When you choose Japanese in the setup program, it downloads the Japanese
data from TEKITO's release on GitHub (github.com) and checks it against the
checksum it carries. That request is the setup program's only network use; it sends nothing
about you beyond what any download does, such as your IP address, and
GitHub's privacy terms apply to it. Installing from a ZIP you downloaded
yourself (`install.ps1 -JapaneseDataPath`) makes no request at all.

## Postal codes

Turning on **Postal codes to addresses** in Settings asks first, then
downloads Japan Post's postal code file (utf_ken_all.zip): the copy on
TEKITO's releases on GitHub (github.com), updated monthly, or Japan Post's
own (www.post.japanpost.jp) when that copy cannot be had. The request sends
nothing about you beyond what any download does, such as your IP address,
and GitHub's or Japan Post's privacy terms apply to it. **Use a file already downloaded**
reads a file you choose on your computer and makes no request.

---

## プライバシー（日本語）

TEKITO の処理はすべて PC の中で行われます。入力した内容、ユーザー辞書、学習した内容をどこにも送りません。アカウント、テレメトリ、更新確認はなく、入力の処理経路でネットワークを使うこともありません。ダウンロードするのは、インストール時の日本語データと、設定で指示した郵便番号データだけです。

- 設定・ユーザー辞書・学習内容は `%LOCALAPPDATA%\TEKITO\user.db`（と、その横の `user.db-wal`、`user.db-shm`）に保存されます。学習しているのは「どの単語でどの候補を選んだか」と、日本語では「どの読みにどの変換を選んだか」の回数だけで、文章や入力履歴は保存しません。日本語の変換は文全体を読むために直前に確定した数語をメモリーにだけ持ちますが、続けて打っている間だけ使い、カーソルがほかへ移ると忘れます。保存はしません。Ctrl+Backspace で確定を取り消せるよう、確定したばかりの文字も次に打ち始めるまでメモリーにだけ持ちます。
- 誤変換を報告できるよう、直近30件の日本語の変換（読みと変換結果のみ。押したキーは保存せず、プライベートウィンドウでの変換も保存しません）を `user.db` に保存します。設定の「情報 → 誤変換の報告」でオフにでき、オフにすると削除されます。「報告」はブラウザで GitHub の Issue フォームを開き、読みと変換結果を入力した状態にするだけで、送信するまで何も送られません。「不具合の報告」も同じように、バージョンと設定を入力した状態でフォームを開きます。
- 再変換（文字を選んで変換キー）は選んだ文字をこの PC の中で読むだけで、ほかの IME の単語の読み込みは選んだファイルをこの PC の中で取り込むだけです。どちらも何も送りません。
- スタートメニューの検索、設定アプリ、ストアアプリでも使えるように、インストーラーはこれらのサンドボックスのアプリに、プログラムのフォルダ（`%LOCALAPPDATA%\Programs\TEKITO`）と言語データ（`%LOCALAPPDATA%\TEKITO\data`）の読み取りだけを許可します。`user.db` は対象外なので、辞書や学習内容は読めません。そこでは TEKITO は既定の設定で動きます。
- インストール時に日本語を選択すると、日本語データを GitHub の TEKITO のリリースからダウンロードし、チェックサムを照合します。インストーラーがネットワークを使用するのはこのときのみです。自分でダウンロードした ZIP からインストールする場合（`install.ps1 -JapaneseDataPath`）は通信しません。
- 設定の「郵便番号から住所に変換」をオンにすると、確認のうえで日本郵便の郵便番号データ（utf_ken_all.zip）をダウンロードします。取得先は GitHub の TEKITO のリリースにある毎月更新の写し（github.com）で、取得できないときは日本郵便のサイト（www.post.japanpost.jp）です。送られるのは IP アドレスなど通常のダウンロードと同じ情報だけです。「ダウンロード済みのファイルを使う」を選んだ場合は、選んだファイルをこの PC の中で取り込むだけで、通信しません。
- 診断ログ（`%TEMP%\TekitoTsf.log`、トレース用ビルドか `TEKITO_TSF_TRACE=1` のときだけ作られます）と設定画面の「診断情報をコピー」には、イベント・時間・エラーコードだけが含まれ、入力した文字は含まれません。
- 学習は設定画面の「辞書と学習 → 選択した候補の学習」でオン/オフでき、「すべて削除」で消せます。「一般 → 詳細 → TEKITO を使わないアプリ」に追加したアプリでは完全にオフになります。
- 「インストールされているアプリ」からアンインストールすると、TEKITO とそのデータ・辞書・学習・設定をすべて削除します。その前に、ユーザー辞書をドキュメントに書き出せます。
- データベースは暗号化していません。ほかの個人ファイルと同じように扱ってください。
