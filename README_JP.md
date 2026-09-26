# TEKITO

Windows のキーボードで英語を楽に打つための入力方式です。Space を押すと打ち間違いを直し、打とうとした単語を候補に出し、打ったとおりでよいときは何もしません。IME（TSF のテキストサービス）として動くので、普通の Windows アプリでそのまま使えます。処理はすべて PC の中で完結します。

[English](README.md)

![TEKITO の候補リスト。ライトとダークのグラス、シンプル表示](docs/images/candidate-list.png)

> TEKITO はプレビュー版です。動作を確認しているのはメモ帳のみです。ほかのアプリの状況は[互換性メモ](docs/compatibility.md)にあります。

## 使い方

普段どおりに打ちます。入力中の単語に下線が付き、その下に候補が出ます。

| キー | 動作 |
| --- | --- |
| Space | 確信のある補正を適用してスペースを入れます。続けて押すと候補を順に送ります。 |
| Shift+Space | ひとつ前の候補に戻ります。 |
| Tab、↑、↓ | 確定せずに候補を移動します。 |
| Backspace | 補正の直後なら、打ったとおりに戻します。 |
| Esc | 打ったとおりにします。 |
| Enter | 候補を選んでいるときはその候補で確定します。それ以外は普通の改行です。 |
| Alt+` | Auto と Direct を切り替えます。Direct では押したキーがそのまま入ります。 |

次の文字を打った時点で単語は確定するので、あとから取り消す操作はありません。知っている単語、名前、コードらしい文字列、URL やアドレス、パスワード・数字・メール用の欄には手を出しません。ターミナルと、設定で追加したアプリでは完全にオフになります。

![TEKITO の設定画面](docs/images/settings.png)

## プライバシー

TEKITO の処理はすべて PC の中で行います。入力した内容をどこにも送らず、アカウントもテレメトリもなく、入力中にネットワークを使いません。選んだ候補から学習した内容は `%LOCALAPPDATA%\TEKITO` に保存され、設定画面から消せます。詳しくは[プライバシーについて](PRIVACY.md)を参照してください。

## インストール

Releases からインストーラーをダウンロードして実行します。Microsoft Edge WebView2 Runtime が必要ですが、Windows 11 には最初から入っています。インストール後にサインアウトしてサインインし直し、Win+Space で TEKITO を選んでください。

## ビルド

Windows 11、C++ デスクトップ開発ワークロード入りの Visual Studio 2026（または 2022）、CMake 3.24 以降、設定画面用の Node.js が必要です。

```powershell
cd apps\tekito-settings\ui
npm ci
npm run build
cd ..\..\..
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
.\scripts\verify.ps1
```

`verify.ps1` はビルド、テスト、[開発のルール](#開発のルール)の確認をまとめて行います。自分のビルドを試すときは、管理者 PowerShell で `scripts\register-debug.ps1` を実行して登録し、`unregister-debug.ps1` で外します。

言語データは `data/` にあります。いちばん大きい 2 つ（地名とフレーズ統計）はリポジトリに入れられないため、`scripts\prepare-full-data-packs.ps1` で公開元からダウンロードして作り直します。詳しくは [Data Packs](docs/data-packs.md) を参照してください。

インストーラーを作るときは `windows-x64-release` プリセットでビルドし、`installer\package-release.ps1` を実行します。

## 構成

| パス | 内容 |
| --- | --- |
| `src/Core` | 候補の検索・順位付けと入力の状態遷移。Windows に依存しません。 |
| `src/Tsf` | TSF テキストサービスと候補ウィンドウ。 |
| `src/UserData` | 設定、ユーザー辞書、学習データ（SQLite）。 |
| `tests` | エンジンとユーザーデータのテスト。 |
| `apps/tekito-settings` | 設定画面。Win32 のホストと WebView2 上の React。 |
| `installer` | セットアップと配布物を作るスクリプト。 |
| `data` | 言語データ。パックごとに manifest と NOTICE があります。 |

## 開発のルール

- 単語やフレーズはデータに置き、`if` 文に書かない。
- 入力の処理経路でネットワークを使わない。診断情報に入力内容を含めない。
- TSF だけで動かす。キーボードフックや擬似入力は使わない。
- 正しい単語、名前、コードはそのままにする。迷ったら置き換えずに候補として出す。

不具合の報告やアイデアは Issues で受け付けています。プレビューの間はプルリクエストを受け付けていません。

## ライセンス

ソースコードは [Functional Source License 1.1](LICENSE.md)（FSL-1.1-ALv2）で公開しています。競合する商用製品に使うこと以外なら、使う・改変する・共有するのは自由です。各リリースは公開から 2 年後に Apache 2.0 になります。配布するインストーラーには [TEKITO 使用許諾契約](installer/TEKITO_LICENSE.md)が、言語データにはそれぞれの出典のライセンスが適用されます。詳しくは[ライセンスについて](LICENSING.md)と[サードパーティー通知](THIRD_PARTY_NOTICES.md)を参照してください。

TEKITO は [Capitata](https://capitata.dev) が開発しています。
