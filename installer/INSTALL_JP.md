# TEKITO のインストール

## インストーラーでインストールする

`TEKITO-0.6.1-full-installer.exe` を実行します。使用許諾に同意すると、インストールが始まります。

- **日本語入力を追加する** を選択すると、日本語の辞書と言語データ（約 80 MB）を GitHub のリリースからダウンロードし、チェックサムを照合してからインストールします。TEKITO は日本語のキーボード一覧に追加されます。
- 日本語を選択しない場合は、英語のキーボード一覧に追加されます。日本語は、インストーラーを再度実行すると追加できます。
- Microsoft Edge WebView2 Runtime が必要です。Windows 11 には標準で含まれています。見つからない場合は、インストーラーに案内を表示します。
- インストール後、サインアウトしてサインインし直すか、使用するアプリを再起動します。
- Win+Space で TEKITO を選択します。モードは、日本語キーボードでは半角/全角、US キーボードでは Alt+` で切り替えます。
- 設定画面は、スタートメニューの「TEKITO Settings」から開きます。

## ZIP から手動でインストールする

`TEKITO-0.6.1-full.zip` を展開し、そのフォルダーで管理者として PowerShell を開いて、次のコマンドを実行します。

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\verify-package.ps1
.\install.ps1
```

日本語も追加する場合は `-Japanese $true` を指定します。オフラインでインストールする場合は、同じリリースの `TEKITO-0.6.1-japanese-data.zip` を事前にダウンロードし、そのパスを指定します（チェックサムも照合します）。

```powershell
.\install.ps1 -Japanese $true
.\install.ps1 -JapaneseDataPath C:\Downloads\TEKITO-0.6.1-japanese-data.zip
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

- 入力内容を外部に送信することはありません。インストーラーがネットワークを使用するのは、日本語を選択したときの日本語データのダウンロードのみです。
- このバージョンにはコード署名がないため、SmartScreen や組織のポリシーで実行が止められることがあります。
- ライセンスは `TEKITO_LICENSE.md` と `LICENSING.md`、個人情報の扱いは `PRIVACY.md` を参照してください。
