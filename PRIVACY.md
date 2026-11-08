# Privacy

TEKITO works entirely on your PC. It does not send what you type, your
dictionary or what it has learned anywhere. It has no account, no telemetry
and no update check, and nothing in the input path uses the network. The
only downloads are the ones you ask for: the Japanese data in the setup
program, the postal code data in Settings, and an AI model in either.

## What TEKITO keeps, and where

| Location | What is in it |
| --- | --- |
| `%LOCALAPPDATA%\TEKITO\user.db` (and `user.db-wal`, `user.db-shm` beside it) | Your settings, your dictionary, what TEKITO has learned from your choices, and your last 30 Japanese conversions while that is on |
| `%LOCALAPPDATA%\TEKITO\data` | The language data packs, and the AI model when you download one |
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
- **Secret mode** (the TEKITO button on the taskbar, or **Settings →
  Dictionary & learning**): until you turn it off, nothing you type is
  learned or kept, in every app. It turns off when you sign out or restart.
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
checksum it carries. With **Check with AI** chosen too, it also downloads the AI model (see
below). These requests are the setup program's only network use; they send nothing
about you beyond what any download does, such as your IP address, and
GitHub's privacy terms apply to them. Installing from a ZIP you downloaded
yourself (`install.ps1 -JapaneseDataPath`) makes no request at all.

## AI models

The AI models are optional. Choosing **Check with AI** in the setup program,
or a model under **Settings → AI** (which asks first), downloads the model and what runs it from TEKITO's release on GitHub
(github.com) and checks them against the checksums Settings carries. The
request sends nothing about you beyond what any download does, such as your
IP address, and GitHub's privacy terms apply to it. The model then runs on
your PC, on the CPU or the GPU, in a separate TEKITO process; what you type
is passed to it on your PC only and is not saved. **Remove the model**
deletes it.

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

TEKITO の処理はすべて PC の中で行われます。入力内容、ユーザー辞書、学習内容が外部に送信されることはありません。アカウント、テレメトリ、更新確認はなく、入力の処理経路でネットワークが使用されることもありません。ダウンロードが行われるのは、インストール時の日本語データ、設定で選択した郵便番号データ、インストール時または設定で選択した AI モデルのみです。

- 設定・ユーザー辞書・学習内容は `%LOCALAPPDATA%\TEKITO\user.db`（と、その横の `user.db-wal`、`user.db-shm`）に保存されます。学習されるのは「どの単語でどの候補を選んだか」と、日本語では「どの読みにどの変換を選んだか」の回数のみで、文章や入力履歴は保存されません。日本語の変換では、文全体を読むために、直前に確定した数語がメモリーにのみ保持されます。続けて入力している間だけ使用され、カーソルがほかの位置に移ると破棄されます。保存はされません。Ctrl+Backspace で確定を取り消せるように、確定したばかりの文字も、次に入力を始めるまでメモリーにのみ保持されます。
- 誤変換を報告できるように、直近30件の日本語の変換（読みと変換結果のみ。押したキーは保存されず、プライベートウィンドウでの変換も保存されません）が `user.db` に保存されます。設定の「情報 → 誤変換の報告」でオフにでき、オフにすると削除されます。「報告」を選択すると、読みと変換結果が入力された状態で、ブラウザに GitHub の Issue フォームが表示されます。送信するまで何も送信されません。「不具合の報告」でも同じように、バージョンと設定が入力された状態でフォームが表示されます。
- 再変換（文字を選択して変換キー）では、選択した文字がこの PC の中で読み取られるだけです。ほかの IME の単語の読み込みでも、選択したファイルがこの PC の中で取り込まれるだけです。どちらも何も送信されません。
- スタートメニューの検索、設定アプリ、ストアアプリでも使用できるように、インストーラーによって、これらのサンドボックスのアプリに、プログラムのフォルダ（`%LOCALAPPDATA%\Programs\TEKITO`）と言語データ（`%LOCALAPPDATA%\TEKITO\data`）の読み取りのみが許可されます。`user.db` は対象外のため、辞書や学習内容は読み取られません。これらのアプリでは、TEKITO は既定の設定で動作します。
- インストール時に日本語を選択すると、日本語データが GitHub の TEKITO のリリースからダウンロードされ、チェックサムが照合されます。「AI で変換を見直す」も選択した場合は、AI モデルもダウンロードされます（後述）。インストーラーがネットワークを使用するのはこれらのときのみです。自分でダウンロードした ZIP からインストールする場合（`install.ps1 -JapaneseDataPath`）は、通信は行われません。
- 設定の「郵便番号から住所に変換」を有効にすると、確認のあとで日本郵便の郵便番号データ（utf_ken_all.zip）がダウンロードされます。取得先は GitHub の TEKITO のリリースにある毎月更新の写し（github.com）で、取得できない場合は日本郵便のサイト（www.post.japanpost.jp）です。送信されるのは、IP アドレスなど通常のダウンロードと同じ情報のみです。「ダウンロード済みのファイルを使う」を選択した場合は、選択したファイルがこの PC の中で取り込まれるだけで、通信は行われません。
- AI モデルは任意です。インストール時に「AI で変換を見直す」を選択するか、設定の「AI」でモデルを選択すると（設定では確認が表示されます）、モデルとその実行に必要なファイルが GitHub の TEKITO のリリース（github.com）からダウンロードされ、チェックサムが照合されます。送信されるのは、IP アドレスなど通常のダウンロードと同じ情報のみです。AI は TEKITO の別のプロセスとして、この PC の CPU または GPU で動作します。入力内容は PC の中でのみ受け渡され、保存されません。「モデルを削除」で削除できます。
- 診断ログ（`%TEMP%\TekitoTsf.log`。トレース用ビルドか `TEKITO_TSF_TRACE=1` のときのみ作成されます）と、設定画面の「診断情報をコピー」には、イベント・時間・エラーコードのみが含まれ、入力した文字は含まれません。
- 学習は、設定画面の「辞書と学習 → 選択した候補の学習」でオン／オフでき、「すべて削除」で削除できます。「一般 → 詳細 → TEKITO を使わないアプリ」に追加したアプリでは、TEKITO は完全にオフになります。
- シークレットモード（タスクバーの TEKITO ボタン、または設定の「辞書と学習」）をオンにすると、オフにするまで、すべてのアプリで入力内容が学習・保存されません。サインアウトや再起動をすると、オフに戻ります。
- 「インストールされているアプリ」からアンインストールすると、TEKITO とそのデータ・辞書・学習内容・設定がすべて削除されます。その前に、ユーザー辞書をドキュメントに書き出せます。
- データベースは暗号化されていません。ほかの個人ファイルと同じように扱ってください。
