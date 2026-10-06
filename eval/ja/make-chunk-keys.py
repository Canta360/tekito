"""Writes eval/ja/chunk_keys.tsv: Japanese and English typed in chunks, a
Space after each, for `tekito_ja_eval --chunks`.

Keys are the chunks as typed, separated by spaces; an English chunk ends
in "*" (the answer, not typed). Expected texts as in make-mixed-keys.py:
{GitHub|github} is either spelling.

Sources:
  chunk-english  English that cannot be read as romaji (github, push)
  chunk-both     English that reads as romaji too (api, data, game)
  chunk-ja       Japanese that looks like English (made, sake, take)
"""

import importlib.util
import pathlib

here = pathlib.Path(__file__).parent
spec = importlib.util.spec_from_file_location("mixed", here / "make-mixed-keys.py")
mixed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mixed)

ENGLISH = [
    ("korewoatode github* ni push* suru", "これを後で{GitHub|github}に{push|プッシュ}する", "これをあとで{GitHub|github}に{push|プッシュ}する"),
    ("kyouha google* de kensakusita", "今日は{Google|google}で検索した"),
    ("slack* de houkokusimasu", "{Slack|slack}で報告します"),
    ("zoom* no kaigini irimasu", "{Zoom|zoom}の会議に入ります"),
    ("excel* no fairuwo okurimasu", "{Excel|excel}のファイルを送ります"),
    ("python* de script* wo kaita", "{Python|python}で{script|スクリプト}を書いた"),
    ("docker* wo tukatteimasu", "{Docker|docker}を使っています"),
    ("mail* wo kakunowo wasureta", "{mail|メール}を書くのを忘れた"),
    ("commit* sitekara push* site", "{commit|コミット}してから{push|プッシュ}して"),
    ("branch* wo kiru hituyougaaru", "{branch|ブランチ}を切る必要がある"),
    ("review* wo onegaisimasu", "{review|レビュー}をお願いします"),
    ("bug* wo naosita", "{bug|バグ}を直した"),
    ("test* ga toorimasita", "{test|テスト}が通りました"),
    ("deploy* ha asitano yoteidesu", "{deploy|デプロイ}は明日の予定です"),
    ("chatgpt* ni kiitemita", "{ChatGPT|chatgpt}に聞いてみた"),
    ("iphone* wo kaikaeta", "{iPhone|iphone}を買い替えた"),
    ("windows* no update* ga owaranai", "{Windows|windows}の{update|アップデート}が終わらない"),
    ("chrome* ga omoi", "{Chrome|chrome}が重い"),
    ("twitter* de mita nyuusu", "{Twitter|twitter}で見たニュース"),
    ("json* wo parse* suru", "{JSON|json}を{parse|パース}する"),
    ("server* wo saikidousita", "{server|サーバー}を再起動した"),
    ("password* wo wasureta", "{password|パスワード}を忘れた"),
    ("link* wo okurune", "{link|リンク}を送るね"),
    ("download* sitekudasai", "{download|ダウンロード}してください"),
    ("schedule* wo chouseisimasu", "{schedule|スケジュール}を調整します"),
    ("meeting* no maeni siryouwo yomu", "{meeting|ミーティング}の前に資料を読む"),
    ("figma* de dezainnsita", "{Figma|figma}でデザインした"),
    ("friday* ni kaigiga aru", "{Friday|friday}に会議がある"),
    ("sorry* osokunatta", "{sorry|ソーリー}遅くなった"),
    ("check* siteoite", "{check|チェック}しておいて"),
    ("spreadsheet* ni kakikonde", "{spreadsheet|スプレッドシート}に書き込んで"),
    ("backup* wo toru", "{backup|バックアップ}を取る"),
    ("qiita* ni kijiwo kaita", "{Qiita|qiita}に記事を書いた"),
    ("stack* overflow* de sirabeta", "{Stack Overflow|stack overflow}で調べた"),
    ("vscode* wo kidousuru", "{VS Code|vscode}を起動する"),
    ("github* actions* ga ugokanai", "{GitHub Actions|github actions|GitHub actions}が動かない"),
    ("pull* request* wo dasita", "{pull request|プルリクエスト}を出した"),
    ("merge* sitemo iidesuka", "{merge|マージ}してもいいですか"),
    ("thanks* matanee", "{thanks|サンクス}またねー", "{thanks|サンクス}またねえ"),
    ("happy* birthday* omedetou", "{happy birthday|Happy birthday}おめでとう"),
    ("weekend* ha nanisuru", "{weekend|ウィークエンド}は何する"),
    ("file* wo share* simasu", "{file|ファイル}を{share|シェア}します"),
    ("screenshot* wo tottekudasai", "{screenshot|スクリーンショット}を撮ってください"),
    ("discord* ni sankasita", "{Discord|discord}に参加した"),
    ("spotify* de kiiteru", "{Spotify|spotify}で聞いてる"),
]

BOTH = [
    ("api* wo tatakuto", "{API|api}を叩くと"),
    ("notion* ni memosita", "{Notion|notion}にメモした"),
    ("youtube* de mita", "{YouTube|youtube}で見た"),
    ("data* wo kousinsuru", "{data|データ}を更新する"),
    ("game* wo suru", "{game|ゲーム}をする"),
    ("online* de sankasuru", "{online|オンライン}で参加する"),
    ("line* deno tyakusin", "{LINE|line}での着信"),
    ("token* wo hakkousuru", "{token|トークン}を発行する"),
    ("amazon* de katta", "{Amazon|amazon}で買った"),
    ("ruby* no baajon", "{Ruby|ruby}のバージョン"),
    ("home* wo hiraku", "{home|ホーム}を開く"),
    ("menu* de erabu", "{menu|メニュー}で選ぶ"),
]

JAPANESE = [
    ("kokomade kita", "ここまで来た"),
    ("sakewo nomu", "酒を飲む"),
    ("takenokowo tabeta", "筍を食べた", "たけのこを食べた", "タケノコを食べた"),
    ("samega oyogu", "鮫が泳ぐ", "サメが泳ぐ"),
    ("anohitoha dare", "あの人は誰"),
    ("imamade arigatou", "今までありがとう"),
    ("namaewo kaite", "名前を書いて"),
    ("tanakasanto aimasu", "田中さんと会います"),
    ("kinouha amedesita", "昨日は雨でした"),
    ("soreha mou naidesu", "それはもうないです"),
    ("made", "まで"),
    ("sake", "酒"),
    ("take", "竹", "武"),
    ("mine", "峰", "ミネ"),
    ("same", "鮫", "サメ"),
    ("tone", "トーン", "利根"),
    ("hide", "秀", "ひで"),
    ("kite", "来て", "着て"),
    ("ane", "姉"),
    ("hate", "果て"),
]


def main() -> None:
    lines = []
    for source, rows in (("chunk-english", ENGLISH), ("chunk-both", BOTH), ("chunk-ja", JAPANESE)):
        for number, (keys, *templates) in enumerate(rows, 1):
            expected = []
            for template in templates:
                for text in mixed.expand(template):
                    if text not in expected:
                        expected.append(text)
            lines.append("\t".join([source, str(number), keys, *expected]))
    out = here / "chunk_keys.tsv"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(lines)} rows -> {out}")


if __name__ == "__main__":
    main()
