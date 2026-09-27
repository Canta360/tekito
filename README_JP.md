<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/header-dark.png">
    <img src="docs/images/header-light.png" alt="TEKITO, a simple input method" width="600">
  </picture>
</p>

<p align="center">
  Windows のキーボードで、英語を楽に打つための入力方式。<br>
  打ち間違いは Space で直り、打ちたかった単語はすぐそこに。打ったとおりでいいときは何もしません。
</p>

<p align="center">
  <a href="README.md">English</a> ·
  <a href="#インストール">インストール</a> ·
  <a href="#ビルド">ビルド</a> ·
  <a href="https://capitata.dev">capitata.dev</a>
</p>

<p align="center">
  <img src="docs/images/typing-demo.png" alt="「thnaks for teh reveiw.」と打つと、単語ごとに候補リストが出て、Space で打ち間違いが直り、「thanks for the review.」になる様子" width="640">
</p>

TEKITO は IME（TSF のテキストサービス）なので、普通の Windows アプリでそのまま使えます。処理はすべて PC の中で完結します。

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

次の文字を打った時点で単語は確定するので、あとから取り消す操作はありません。知っている単語、「brb」のようなチャットの略語、自分で大文字にして打った語、コードらしい文字列、URL やアドレス、パスワード・数字・メール用の欄には手を出しません。

![TEKITO の候補リスト。ライトとダークのグラス、シンプル表示](docs/images/candidate-list.png)

## Auto と Direct

TEKITO には 2 つのモードがあります。タスクバーのボタンが今のモードを表していて、クリックするか Alt+` で切り替えられます。

<table>
  <tr>
    <td width="72" align="center"><img src="docs/images/mode-auto.png" width="48" alt=""></td>
    <td><b>Auto</b><br>スペルを直し、打ちながら単語を提案します。</td>
    <td><img src="docs/images/taskbar-auto.png" width="216" alt="Auto のときのタスクバー"></td>
  </tr>
  <tr>
    <td align="center">
      <picture>
        <source media="(prefers-color-scheme: dark)" srcset="docs/images/mode-direct-dark.png">
        <img src="docs/images/mode-direct-light.png" width="48" alt="">
      </picture>
    </td>
    <td><b>Direct</b><br>押したキーがそのまま入ります。</td>
    <td><img src="docs/images/taskbar-direct.png" width="216" alt="Direct のときのタスクバー"></td>
  </tr>
</table>

ターミナル、管理者として実行中のアプリ、設定で追加したアプリでは、TEKITO はオフになります。

![TEKITO の設定画面](docs/images/settings.png)

設定画面、インストーラー、TEKITO のメッセージは日本語と英語に対応しています。通常は Windows の表示言語に合わせて表示され、**設定 → 一般 → 表示言語** で切り替えられます。

## プライバシー

TEKITO の処理はすべて PC の中で行います。入力した内容をどこにも送らず、アカウントもテレメトリもなく、入力中にネットワークを使いません。選んだ候補から学習した内容は `%LOCALAPPDATA%\TEKITO` に保存され、設定画面から消せます。詳しくは[プライバシーについて](PRIVACY.md)を参照してください。

## インストール

[Releases](https://github.com/Canta360/tekito/releases/latest) から `TEKITO-0.1.0-full-installer.exe` をダウンロードして実行します。Microsoft Edge WebView2 Runtime が必要ですが、Windows 11 には最初から入っています。インストール後は使いたいアプリを再起動するか、サインアウトしてサインインし直してから、Win+Space で TEKITO を選んでください。

インストーラーはまだコード署名をしていないため、Windows SmartScreen が実行の確認を求めることがあります。

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

言語データは `data/` にあります。フレーズ統計のパックだけはリポジトリに入れられない大きさなので、`scripts\prepare-full-data-packs.ps1` で公開元からダウンロードして作り直します。なくても TEKITO とテストは動きます。詳しくは [Data Packs](docs/data-packs.md) を参照してください。

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

<p align="center">
  <a href="https://capitata.dev">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="docs/images/capitata-dark.png">
      <img src="docs/images/capitata-light.png" alt="Capitata" width="160">
    </picture>
  </a>
</p>
