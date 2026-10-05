# TEKITO のインストール

## いちばん簡単な方法

`TEKITO-0.4.0-full-installer.exe` を実行します。使用許諾への同意を求められたあと、自動でインストールされます。

- 「日本語入力を追加する」を選ぶと、日本語の辞書と言語データ（約 48 MB）を GitHub のリリースからダウンロードし、チェックサムを確かめてからインストールします。TEKITO は日本語のキーボード一覧に入ります。
- 日本語を選ばない場合は、英語のキーボード一覧に入ります。あとからインストーラーを実行し直せば日本語を追加できます。
- Microsoft Edge WebView2 Runtime が必要です。Windows 11 には最初から入っています。見つからない場合はインストーラーが案内します。
- 終わったらサインアウトしてサインインし直すか、使いたいアプリを再起動してください。
- Win+Space で TEKITO を選びます。日本語キーボードなら半角/全角、US キーボードなら Alt+` でモードを切り替えられます。
- 設定はスタートメニューの「TEKITO Settings」から開けます。

## ZIP から手動でインストールする

`TEKITO-0.4.0-full.zip` を展開し、そのフォルダで管理者 PowerShell を開いて次を実行します。

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\verify-package.ps1
.\install.ps1
```

日本語も入れるときは `-Japanese $true` を付けます。ネットワークを使わずに入れる場合は、同じリリースの `TEKITO-0.4.0-japanese-data.zip` を先にダウンロードしておき、その場所を指定します（チェックサムは同じように確かめます）。

```powershell
.\install.ps1 -Japanese $true
.\install.ps1 -JapaneseDataPath C:\Downloads\TEKITO-0.4.0-japanese-data.zip
```

英語のキーボード一覧にも TEKITO を残すときは `-KeepEnglishProfile $true` を付けます。

インストール後の状態は次のコマンドで確認できます（何も変更しません）。

```powershell
.\scripts\verify-installed.ps1
```

プログラムは `%LOCALAPPDATA%\Programs\TEKITO` に、言語データは `%LOCALAPPDATA%\TEKITO\data` に入ります。ユーザー辞書・学習データ・設定は `%LOCALAPPDATA%\TEKITO\user.db` に保存されます。

## アンインストール

展開したフォルダで、管理者 PowerShell から実行します。

```powershell
.\uninstall.ps1
```

日本語と英語、両方のキーボード一覧から TEKITO を外します。ユーザー辞書・学習データ・設定は残ります。それらも消すときだけ `-RemoveUserData` を付けてください。

## 注意

- 入力した内容をネットワークに送ることはありません。インストーラーがネットワークを使うのは、日本語を選んだときに日本語データをダウンロードするときだけです。
- このバージョンはコード署名がないため、SmartScreen や組織のポリシーで止められることがあります。
- ライセンスは `TEKITO_LICENSE.md` と `LICENSING.md`、個人情報の扱いは `PRIVACY.md` を参照してください。
