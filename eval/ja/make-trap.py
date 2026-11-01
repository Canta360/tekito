"""Writes eval/ja/trap.jsonl: Mix sentences whose keys look like English.

trap-ja: Japanese whose romaji spells an English word (sake, made, are,
sore, ten) or is a Japanese word English took over (sushi, karate, tsunami),
mostly as a chunk of its own, the way it is hardest to judge. trap-en: the
same spellings meant as English. Written for TEKITO; no text is
copied. Rows: text | chunks (kana; @ before English) | other right texts.

  python eval/ja/make-trap.py

It writes trap.jsonl and, through kana-to-keys.py, trap_keys.tsv (sources
fresh-trap-ja and fresh-trap-en, numbered 1 on).
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

JAPANESE = """
酒を飲みすぎた|さけ を のみすぎた
鮭を焼いて食べた|さけ を やいて たべた|サケを焼いて食べた
鮫が海を泳いでいる|さめ が うみを およいでいる|サメが海を泳いでいる
竹を切って箸を作った|たけ を きって はしを つくった
来てくれてありがとう|きて くれて ありがとう
駅まで迎えに行くね|えき まで むかえに いくね
峰の向こうに夕日が沈む|みね の むこうに ゆうひが しずむ
道の果てに小さな村がある|みちの はて に ちいさな むらが ある
姉と一緒に買い物に行った|あね と いっしょに かいものに いった
誰が来るのか分からない|だれ が くるのか わからない|誰が来るのかわからない|誰がくるのか分からない
彼が先に帰りました|かれ が さきに かえりました
顎が痛くて食べられない|あご が いたくて たべられない
あれを取ってくれる？|あれ を とって くれる？
物は試しにやってみよう|もの は ためしに やってみよう|ものは試しにやってみよう
駄目なら別の方法を考える|だめ なら べつの ほうほうを かんがえる|ダメなら別の方法を考える|だめなら別の方法を考える
骨が折れる仕事だった|ほね が おれる しごとだった
将来のために貯金している|しょうらいの ため に ちょきんしている|将来の為に貯金している
ちょっと待てよ|ちょっと まて よ
そこを右に曲がって|そこ を みぎに まがって
訳を聞かせてほしい|わけ を きかせて ほしい|訳を聞かせて欲しい
今回は負けを認める|こんかいは まけ を みとめる
さて、そろそろ始めようか|さて、 そろそろ はじめようか
見て、虹が出てる|みて、 にじが でてる
点を取った|てん を とった
変な夢を見た|へん な ゆめを みた
万が一に備える|まん が いちに そなえる
麺が伸びてしまった|めん が のびて しまった
金のメダルを取った|きん の めだるを とった
番を待っている|ばん を まっている
損をしたくない|そん を したくない
群を抜いて速い|ぐん を ぬいて はやい
文を短くまとめる|ぶん を みじかく まとめる
瓶の蓋が開かない|びん の ふたが あかない
雨が急に降ってきた|あめ が きゅうに ふってきた
亀がゆっくり歩く|かめ が ゆっくり あるく
豆を煮ている|まめ を にている
種をまいた|たね を まいた|種を蒔いた
羽根が落ちていた|はね が おちていた|羽が落ちていた
人の真似をする|ひとの まね を する
バネが壊れた|ばね が こわれた
金がない|かね が ない|金が無い
俺が行くよ|おれ が いくよ
情報の漏れがないか確かめる|じょうほうの もれ が ないか たしかめる|情報の漏れが無いか確かめる
それは違うと思う|それ は ちがうと おもう
どれを選べばいい？|どれ を えらべば いい？
早く取れよ|はやく とれ よ|早くとれよ
伊達政宗の像を見た|だて まさむねの ぞうを みた
当てが外れた|あて が はずれた
食べてみて|たべて みて
糧になる経験だった|かて に なる けいけんだった
ちょっと乗せてくれる？|ちょっと のせ て くれる？
早く干せよ|はやく ほせ よ
褒めて伸ばす方針です|ほめ て のばす ほうしんです
案を三つ出した|あん を みっつ だした|案を3つ出した
恩を返したい|おん を かえしたい
ヨガを始めた|よが を はじめた
土手を散歩した|どて を さんぽした
寿司を食べに行こう|すし を たべに いこう
カラオケに行った|からおけ に いった
アニメを見た|あにめ を みた
布団を干した|ふとん を ほした
着物を着た|きもの を きた
津波に注意してください|つなみ に ちゅういしてください
漫画を読んでいる|まんが を よんでいる|マンガを読んでいる
侍の映画を見た|さむらい の えいがを みた
忍者が好きだ|にんじゃ が すきだ
禅の考え方を学ぶ|ぜん の かんがえかたを まなぶ
絵文字を使う|えもじ を つかう
改善を続ける|かいぜん を つづける
盆栽を育てている|ぼんさい を そだてている
空手の練習に行く|からて の れんしゅうに いく
枝豆を食べた|えだまめ を たべた
抹茶のアイスが好き|まっちゃ の あいすが すき
わさびを入れすぎた|わさび を いれすぎた|ワサビを入れすぎた
来て、見て、勝った|きて、 みて、 かった
待て、まだ早い|まて、 まだ はやい
今日はここまで|きょうは ここ まで
来週まで待って|らいしゅう まで まって
そこにいるのは誰|そこに いるのは だれ
彼は医者です|かれ は いしゃです
ほら、あれ見て|ほら、 あれ みて
あの峰を越える|あの みね を こえる
どこまで行くの？|どこ まで いくの？
なぜ駄目なの？|なぜ だめ なの？|なぜダメなの？
鮭のおにぎりを買った|さけ の おにぎりを かった|サケのおにぎりを買った
雨の日は家にいる|あめ の ひは いえに いる
種も仕掛けもない|たね も しかけも ない|種も仕掛けも無い
金を貸してほしい|かね を かして ほしい|金を貸して欲しい
豆まきをした|まめ まきを した|豆撒きをした
姉が結婚した|あね が けっこんした
酒は飲めない|さけ は のめない
竹の子を掘った|たけ の こを ほった|筍を掘った|たけのこを掘った
鮫に気をつけて|さめ に きを つけて|サメに気をつけて|鮫に気を付けて|サメに気を付けて
来て良かった|きて よかった|来てよかった
駅まで歩いた|えき まで あるいた
誰でもできる|だれ でも できる
あれは何？|あれ は なに？
それならいいよ|それ なら いいよ
俺も行きたい|おれ も いきたい
変だと思った|へん だと おもった
麺を茹でる|めん を ゆでる
損はさせない|そん は させない
天の声が聞こえた|てん の こえが きこえた
文の終わりに句点を打つ|ぶん の おわりに くてんを うつ
金曜日に会おう|きん ようびに あおう
二人の番が来た|ふたりの ばん が きた
物を大切にする|もの を たいせつに する|ものを大切にする
骨まで食べられる魚|ほね まで たべられる さかな
将棋のために上京した|しょうぎの ため に じょうきょうした|将棋の為に上京した
"""

ENGLISH = """
For the sake of argument.|@for @the @sake @of @argument.
It is the same as before.|@it @is @the @same @as @before.
Take it easy.|@take @it @easy.
I will take a break.|@i @will @take @a @break.
Made in Japan.|@made @in @japan.
That is mine.|@that @is @mine.
I hate waiting.|@i @hate @waiting.
Are you there?|@are @you @there?
Please make a copy.|@please @make @a @copy.
I will bake a cake.|@i @will @bake @a @cake.
Wake me up at seven.|@wake @me @up @at @seven.
Let us set a date.|@let @us @set @a @date.
The rate is high.|@the @rate @is @high.
I ate too much.|@i @ate @too @much.
Ten minutes left.|@ten @minutes @left.
He is a kind man.|@he @is @a @kind @man.
That was fun.|@that @was @fun.
My son is five.|@my @son @is @five.
More coffee please.|@more @coffee @please.
I have a sore throat.|@i @have @a @sore @throat.
このsiteを見てください|この @site を みてください
nameを入力してください|@name を にゅうりょくしてください
dateの予定を立てる|@date の よていを たてる|デートの予定を立てる
rateが上がった|@rate が あがった
makeのoptionを確認する|@make の @option を かくにんする
"""


def rows(block):
    for line in block.strip().splitlines():
        parts = line.split("|")
        yield {"text": parts[0], "chunks": parts[1].split(), "also": parts[2:]}


here = Path(__file__).parent
lines, keys = [], []
with tempfile.TemporaryDirectory() as work:
    for name, block in (("ja", JAPANESE), ("en", ENGLISH)):
        kind = [dict(row, kind=name) for row in rows(block)]
        lines += [json.dumps(row, ensure_ascii=False) for row in kind]
        part = Path(work) / f"trap_{name}.jsonl"
        part.write_text("\n".join(json.dumps(row, ensure_ascii=False) for row in kind) + "\n", encoding="utf-8")
        tsv = Path(work) / f"trap_{name}_keys.tsv"
        subprocess.run([sys.executable, str(here / "kana-to-keys.py"), str(part), str(tsv), f"trap-{name}"], check=True)
        keys += [line.split("\t") for line in tsv.read_text(encoding="utf-8").splitlines() if line]
(here / "trap.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
for number, fields in enumerate(keys, 1):
    fields[1] = str(number)
with open(here / "trap_keys.tsv", "w", encoding="utf-8", newline="\n") as out:
    out.write("\n".join("\t".join(fields) for fields in keys) + "\n")
print(len(lines), "rows,", len(keys), "with keys")
