# TEKITO のインストール

## インストーラーでインストールする

リリースには次の2種類があります。

- **`TEKITO-<version>-full-installer.exe`** — 英語と本体が入ったオフライン寄りのインストーラー
- **`TEKITO-online-installer.exe`** — 起動すると GitHub から最新のフルインストーラーを取得し、SHA-256 を確認してから実行します。あとから更新を確認するオプションも選べます

フルインストーラーを実行し、使用許諾に同意するとインストールが始まります。

- **日本語入力を追加する** を選択すると、日本語の辞書と言語データ（約 88 MB）を GitHub のリリースからダウンロードし、チェックサムを照合してからインストールします。TEKITO は日本語のキーボード一覧に追加されます。
- 日本語を選択しない場合は、英語のキーボード一覧に追加されます。日本語は、インストーラーを再度実行すると追加できます。
- **オプション → 更新を確認する**（既定オフ）をオンにすると、設定が GitHub で新しい TEKITO を確認できます。入力そのものはネットを使いません。
- Microsoft Edge WebView2 Runtime が必要です。Windows 11 には標準で含まれています。見つからない場合は、インストーラーに案内を表示します。
- インストール後、サインアウトしてサインインし直すか、使用するアプリを再起動します。
- Win+Space で TEKITO を選択します。モードは、日本語キーボードでは半角/全角、US キーボードでは Alt+` で切り替えます。
- 設定画面は、スタートメニューの「TEKITO Settings」から開きます。

## ZIP から手動でインストールする

`TEKITO-0.6.2-full.zip` を展開し、そのフォルダーで管理者として PowerShell を開いて、次のコマンドを実行します。

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\verify-package.ps1
.\install.ps1
```

日本語も追加する場合は `-Japanese $true` を指定します。オフラインでインストールする場合は、同じリリースの `TEKITO-0.6.2-japanese-data.zip` を事前にダウンロードし、そのパスを指定します（チェックサムも照合します）。

```powershell
.\install.ps1 -Japanese $true
.\install.ps1 -JapaneseDataPath C:\Downloads\TEKITO-0.6.2-japanese-data.zip
```

英語のキーボード一覧にも TEKITO を追加する場合は `-KeepEnglishProfile $true` を指定します。

インストール後の状態は、次のコマンドで確認できます（変更は加えません）。

```powershell
.\scripts\verify-installed.ps1
```

プログラムは `%LOCALAPPDATA%\Programs\TEKITO` に、言語データは `%LOCALAPPDATA%\TEKITO\data` にインストールされます。ユーザー辞書・学習データ・設定は `%LOCALAPPDATA%\TEKITO\user.db` に保存されます。

## アンインストール

Windows の「設定」→「アプリ」→「インストールされているアプリ」で TEKITO を選び、「アンインストール」を選択します。ユーザー辞書をドキュメントに書き出すかどうかを選べます。プログラム、言語データ、辞書、学習データ、設定はすべて削除されます。使用中のファイルは、次回のサインイン時に削除されます。

PowerShell から行う場合は、展開したフォルダーで管理者として `.\uninstall.ps1` を実行します。辞書・学習データ・設定も削除する場合は `-RemoveUserData`、先に辞書を書き出す場合は `-ExportTo <フォルダー>` を指定します。

## 注意

- 入力内容を外部に送信することはありません。ネットワークを使うのは、日本語データのダウンロード、オンラインインストーラー／任意の更新確認、および設定で指示した郵便番号データだけです。
- このバージョンにはコード署名がないため、SmartScreen や組織のポリシーで実行が止められることがあります。
- ライセンスは `TEKITO_LICENSE.md` と `LICENSING.md`、個人情報の扱いは `PRIVACY.md` を参照してください。
