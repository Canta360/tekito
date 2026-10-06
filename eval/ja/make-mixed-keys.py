"""Writes eval/ja/mixed_keys.tsv: Japanese typed with English words in it,
as keys for `tekito_ja_eval --keys-corpus`.

Each sentence is typed in romaji in one go, English words as they are
spelled. Expected texts list every acceptable form: {GitHub|github} stands
for either spelling.

Sources:
  mixed-leftover  English that cannot be read as romaji (github, push):
                  should come out in English on its own.
  mixed-clean     English that reads as romaji too (api, notion): Japanese
                  first is fine, the English should be in the list.
  mixed-ja        Japanese that looks like English (made, sake, take): must
                  stay Japanese.
"""

import itertools
import pathlib
import re

LEFTOVER = [
    ("korewoatodegithubnipushsuru", "これを後で{GitHub|github}に{push|プッシュ}する", "これをあとで{GitHub|github}に{push|プッシュ}する"),
    ("kyouhagoogledekensakusita", "今日は{Google|google|グーグル}で検索した"),
    ("slackdehoukokusimasu", "{Slack|slack}で報告します"),
    ("zoomnokaiginiirimasu", "{Zoom|zoom}の会議に入ります"),
    ("excelnofairuwookurimasu", "{Excel|excel}のファイルを送ります"),
    ("pythondescriptwokaita", "{Python|python}で{script|スクリプト}を書いた"),
    ("dockerwotukatteimasu", "{Docker|docker}を使っています"),
    ("mailwokakunowasureta", "{mail|メール}を書くのを忘れた"),
    ("commitsitekarapushsite", "{commit|コミット}してから{push|プッシュ}して"),
    ("branchwokiruhituyougaaru", "{branch|ブランチ}を切る必要がある"),
    ("reviewwoonegaisimasu", "{review|レビュー}をお願いします"),
    ("bugwonaosita", "{bug|バグ}を直した"),
    ("testgatoorimasita", "{test|テスト}が通りました"),
    ("deployhaasitanoyoteidesu", "{deploy|デプロイ}は明日の予定です"),
    ("chatgptnikiitemita", "{ChatGPT|chatgpt}に聞いてみた"),
    ("iphonewokaikaeta", "{iPhone|iphone}を買い替えた"),
    ("windowsnoupdategaowaranai", "{Windows|windows}の{update|アップデート}が終わらない"),
    ("chromegaomoi", "{Chrome|chrome}が重い"),
    ("twitterdemitanyuusu", "{Twitter|twitter}で見たニュース"),
    ("jsonwoparsesuru", "{JSON|json}を{parse|パース}する"),
    ("serverwosaikidousita", "{server|サーバー}を再起動した"),
    ("passwordwowasureta", "{password|パスワード}を忘れた"),
    ("linkwookurune", "{link|リンク}を送るね"),
    ("downloadsitekudasai", "{download|ダウンロード}してください"),
    ("schedulewochouseisimasu", "{schedule|スケジュール}を調整します"),
    ("meetingnomaenisiryouwoyomu", "{meeting|ミーティング}の前に資料を読む"),
    ("figmadedezainnsita", "{Figma|figma}でデザインした"),
    ("fridaynikaigigaaru", "{Friday|friday}に会議がある"),
    ("sorryosokunatta", "{sorry|ソーリー}遅くなった"),
    ("checksiteoite", "{check|チェック}しておいて"),
    ("spreadsheetnikakikonde", "{spreadsheet|スプレッドシート}に書き込んで"),
    ("backupwotoru", "{backup|バックアップ}を取る"),
    ("qiitanikijiwokaita", "{Qiita|qiita}に記事を書いた"),
    ("stackoverflowdesirabeta", "{Stack Overflow|stackoverflow}で調べた"),
    ("vscodewokidousuru", "{VS Code|vscode}を起動する"),
    ("githubactionsgaugokanai", "{GitHub Actions|githubactions}が動かない"),
    ("pullrequestwodasita", "{pull request|pullrequest|プルリクエスト}を出した"),
    ("mergesitemoiidesuka", "{merge|マージ}してもいいですか"),
    ("thanksmatanene", "{thanks|サンクス}またねー", "{thanks|サンクス}またね"),
    ("happybirthdayomedetou", "{happy birthday|happybirthday}おめでとう"),
    ("weekendhananisuru", "{weekend|ウィークエンド}は何する"),
    ("filewosharesimasu", "{file|ファイル}を{share|シェア}します"),
    ("screenshotwotottekudasai", "{screenshot|スクリーンショット}を撮ってください"),
    ("discordnisankasita", "{Discord|discord}に参加した"),
    ("spotifydekiiteru", "{Spotify|spotify}で聞いてる"),
]

CLEAN = [
    ("apiwotatakuto", "{API|api}を叩くと"),
    ("notionnimemosita", "{Notion|notion}にメモした"),
    ("youtubedemita", "{YouTube|youtube|ユーチューブ}で見た"),
    ("datawokousinsuru", "{data|データ}を更新する"),
    ("gamewosuru", "{game|ゲーム}をする"),
    ("onlinedesankasuru", "{online|オンライン}で参加する"),
    ("linedenotyakusin", "{LINE|line|ライン}での着信"),
    ("tokenwohakkousuru", "{token|トークン}を発行する"),
    ("amazondekatta", "{Amazon|amazon|アマゾン}で買った"),
    ("rubynobaajon", "{Ruby|ruby|ルビー}のバージョン"),
    ("homewohiraku", "{home|ホーム}を開く"),
    ("menudeerabu", "{menu|メニュー}で選ぶ"),
]

JAPANESE = [
    ("kokomadekita", "ここまで来た"),
    ("sakewonomu", "酒を飲む"),
    ("takenokowotabeta", "筍を食べた", "たけのこを食べた", "タケノコを食べた"),
    ("samegaoyogu", "鮫が泳ぐ", "サメが泳ぐ"),
    ("anohitohadare", "あの人は誰"),
    ("imamadearigatou", "今までありがとう"),
    ("namaewokaite", "名前を書いて"),
    ("tokoroninoboru", "所に登る", "ところに登る"),
    ("tanakasantoaimasu", "田中さんと会います"),
    ("kinouhaamedesita", "昨日は雨でした"),
    ("sorehamounaidesu", "それはもうないです"),
]


def expand(template: str) -> list[str]:
    parts = re.split(r"(\{[^}]*\})", template)
    choices = [p[1:-1].split("|") if p.startswith("{") else [p] for p in parts]
    return ["".join(c) for c in itertools.product(*choices)]


def main() -> None:
    out = pathlib.Path(__file__).with_name("mixed_keys.tsv")
    lines = []
    for source, rows in (("mixed-leftover", LEFTOVER), ("mixed-clean", CLEAN), ("mixed-ja", JAPANESE)):
        for number, (keys, *templates) in enumerate(rows, 1):
            expected = []
            for template in templates:
                for text in expand(template):
                    if text not in expected:
                        expected.append(text)
            lines.append("\t".join([source, str(number), keys, *expected]))
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(lines)} rows -> {out}")


if __name__ == "__main__":
    main()
