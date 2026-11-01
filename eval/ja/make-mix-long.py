"""Writes eval/ja/mix_long*.jsonl: longer sentences for Mix, typed in chunks.

Each row: the text, then its chunks as a Mix typist presses Space between
them (kana; @ before an English word). Proper names written with a capital
(Google) are also right in lower case.
"""
import json
import re
import sys
from pathlib import Path

MIXED = """
今日のmeetingでreleaseの日程を決めます。|きょうの @meeting で @release の にっていを きめます。
このbranchをmainにmergeしてからdeployしてください。|この @branch を @main に @merge してから @deploy してください。
さっきのbugはもうfixしたのでtestをもう一度回します。|さっきの @bug は もう @fix したので @test を もういちど まわします。
明日のpresentationのslideをGoogleドライブに上げておきました。|あしたの @presentation の @slide を @google どらいぶに あげておきました。
clientからのfeedbackを読んで、仕様を少し変えることにしました。|@client からの @feedback を よんで、 しようを すこし かえることに しました。
Slackで共有したdocumentに目を通しておいてください。|@slack で きょうゆうした @document に めを とおしておいて ください。
来週のscheduleがまだ決まっていないので、調整をお願いします。|らいしゅうの @schedule が まだ きまっていないので、 ちょうせいを おねがいします。
serverの負荷が高いので、cacheを入れて様子を見ます。|@server の ふかが たかいので、 @cache を いれて ようすを みます。
databaseのbackupは毎晩自動で取っています。|@database の @backup は まいばん じどうで とっています。
新しいfeatureのdesignをFigmaで作り直しました。|あたらしい @feature の @design を @figma で つくりなおしました。
reviewが終わったら、そのpull requestをcloseしてください。|@review が おわったら、 その @pull @request を @close してください。
今回のupdateでloginの画面が大きく変わります。|こんかいの @update で @login の がめんが おおきく かわります。
errorのlogを見ると、timeoutが原因のようです。|@error の @log を みると、 @timeout が げんいんの ようです。
週末にPythonでscriptを書いて、作業を自動化しました。|しゅうまつに @python で @script を かいて、 さぎょうを じどうかしました。
deadlineに間に合わないかもしれないので、priorityを見直しましょう。|@deadline に まにあわないかも しれないので、 @priority を みなおしましょう。
Zoomのlinkをcalendarに貼っておきました。|@zoom の @link を @calendar に はっておきました。
このissueはsupportの担当に回してください。|この @issue は @support の たんとうに まわしてください。
sampleのcodeを動かしてみたら、すぐにcrashしました。|@sample の @code を うごかしてみたら、 すぐに @crash しました。
projectのmemberが増えたので、channelを分けることにしました。|@project の @member が ふえたので、 @channel を わけることに しました。
昨日のsessionはとても参考になったので、recordingを共有します。|きのうの @session は とても さんこうに なったので、 @recording を きょうゆうします。
browserのcacheを消してから、もう一度reloadしてみてください。|@browser の @cache を けしてから、 もういちど @reload してみてください。
その件はemailではなくchatで相談しましょう。|その けんは @email ではなく @chat で そうだんしましょう。
testのcoverageが下がっているので、追加でcaseを書きます。|@test の @coverage が さがっているので、 ついかで @case を かきます。
毎朝のstandupで進み具合を共有しています。|まいあさの @standup で すすみぐあいを きょうゆうしています。
contractの内容をlegalにcheckしてもらいます。|@contract の ないようを @legal に @check してもらいます。
このpageはmobileで見ると表示が崩れます。|この @page は @mobile で みると ひょうじが くずれます。
versionを上げたら、pluginが動かなくなりました。|@version を あげたら、 @plugin が うごかなく なりました。
夏休みの前にbudgetの申請を出しておきたいです。|なつやすみの まえに @budget の しんせいを だしておきたいです。
marketingのteamと一緒にcampaignの案を考えています。|@marketing の @team と いっしょに @campaign の あんを かんがえています。
今日はremoteで働いているので、callで話しましょう。|きょうは @remote で はたらいているので、 @call で はなしましょう。
このtableのcolumnを一つ増やしてほしいです。|この @table の @column を ひとつ ふやして ほしいです。
passwordを忘れたので、resetのmailを送ってもらえますか。|@password を わすれたので、 @reset の @mail を おくって もらえますか。
dashboardの数字が昨日から更新されていません。|@dashboard の すうじが きのうから こうしんされていません。
この機能はbetaなので、feedbackを歓迎します。|この きのうは @beta なので、 @feedback を かんげいします。
onboardingの資料を新しい人向けに書き直しました。|@onboarding の しりょうを あたらしい ひとむけに かきなおしました。
金曜日までにdraftを送るので、commentをお願いします。|きんようびまでに @draft を おくるので、 @comment を おねがいします。
networkが遅いせいで、uploadに時間がかかっています。|@network が おそい せいで、 @upload に じかんが かかっています。
定例のagendaに今回のincidentを入れておきます。|ていれいの @agenda に こんかいの @incident を いれておきます。
このsiteはsearchの結果がとても見やすいです。|この @site は @search の けっかが とても みやすいです。
shopのopenに合わせてsaleを始める予定です。|@shop の @open に あわせて @sale を はじめる よていです。
新しいlaptopが届いたので、環境のsetupをしています。|あたらしい @laptop が とどいたので、 かんきょうの @setup を しています。
このtaskは来週のsprintに回してもいいですか。|この @task は らいしゅうの @sprint に まわしても いいですか。
Excelのfileを開くと、文字が化けてしまいます。|@excel の @file を ひらくと、 もじが ばけて しまいます。
interviewの日程が決まったら、roomを予約します。|@interview の にっていが きまったら、 @room を よやくします。
主なtargetは二十代の女性です。|おもな @target は にじゅうだいの じょせいです。|20代
このplanだと、costが高すぎると思います。|この @plan だと、 @cost が たかすぎると おもいます。
sizeが合わなかったので、別のcolorに交換しました。|@size が あわなかったので、 べつの @color に こうかんしました。
menuにある新しいitemを試してみました。|@menu に ある あたらしい @item を ためして みました。
tutorialを最後までやれば、基本は分かります。|@tutorial を さいごまで やれば、 きほんは わかります。
このgameはonlineで友達と遊べます。|この @game は @online で ともだちと あそべます。
来月からofficeの場所が変わります。|らいげつから @office の ばしょが かわります。
部長のapprovalが出たので、orderを進めます。|ぶちょうの @approval が でたので、 @order を すすめます。
このstepを飛ばすと、installに失敗します。|この @step を とばすと、 @install に しっぱいします。
storyの順番を入れ替えたら、話が分かりやすくなりました。|@story の じゅんばんを いれかえたら、 はなしが わかりやすく なりました。
cameraの設定を変えたら、photoが明るくなりました。|@camera の せっていを かえたら、 @photo が あかるく なりました。
このquestionには後でまとめて答えます。|この @question には あとで まとめて こたえます。
trainが遅れているので、meetingに少し遅れます。|@train が おくれているので、 @meeting に すこし おくれます。
hotelの予約はwebで済ませました。|@hotel の よやくは @web で すませました。
このformに名前とaddressを入れてください。|この @form に なまえと @address を いれてください。
trafficが増えたので、serverを一台足しました。|@traffic が ふえたので、 @server を いちだい たしました。|1台
このcommandを打つと、全部のcontainerが止まります。|この @command を うつと、 ぜんぶの @container が とまります。
Dockerのimageが大きすぎて、buildに時間がかかります。|@docker の @image が おおきすぎて、 @build に じかんが かかります。
来週のeventでdemoを見せる予定です。|らいしゅうの @event で @demo を みせる よていです。
このcategoryの売り上げは先月より伸びています。|この @category の うりあげは せんげつより のびています。|売上げ
phoneの電池がすぐに切れるので、新しいcaseを買いました。|@phone の でんちが すぐに きれるので、 あたらしい @case を かいました。
このlistから好きなoptionを選んでください。|この @list から すきな @option を えらんでください。
recipeの通りに作ったのに、味が全然違います。|@recipe の とおりに つくったのに、 あじが ぜんぜん ちがいます。
workshopの参加者にsurveyを送りました。|@workshop の さんかしゃに @survey を おくりました。
このmodelは精度が高いけれど、speedが遅いです。|この @model は せいどが たかいけれど、 @speed が おそいです。
新しいlogoのcolorについて意見をください。|あたらしい @logo の @color について いけんを ください。
今夜のmatchは友達とbarで見る予定です。|こんやの @match は ともだちと @bar で みる よていです。
musicを聞きながら作業すると集中できます。|@music を ききながら さぎょうすると しゅうちゅうできます。
このprojectのgoalをもう一度確認しましょう。|この @project の @goal を もういちど かくにんしましょう。
releaseのnoteに変更点をまとめておきました。|@release の @note に へんこうてんを まとめておきました。
userの声を集めるために、formを作りました。|@user の こえを あつめるために、 @form を つくりました。
このkeyを押すと、windowが閉じます。|この @key を おすと、 @window が とじます。
printerの紙が切れているので、補充してください。|@printer の かみが きれているので、 ほじゅうしてください。
年末は休みが多いので、deliveryの日を早めます。|ねんまつは やすみが おおいので、 @delivery の ひを はやめます。
このbookはbeginnerにもおすすめです。|この @book は @beginner にも おすすめです。|お勧めです
courseの途中でもcancelできますか。|@course の とちゅうでも @cancel できますか。
自分のprofileの写真を変えました。|じぶんの @profile の しゃしんを かえました。
このvideoは字幕をonにすると見やすいです。|この @video は じまくを @on に すると みやすいです。
accountを作るときに、phoneの番号が必要です。|@account を つくるときに、 @phone の ばんごうが ひつようです。
queueに溜まったjobを順番に処理しています。|@queue に たまった @job を じゅんばんに しょりしています。
このfunctionは引数が多すぎて読みにくいです。|この @function は ひきすうが おおすぎて よみにくいです。
reportのformatを統一してほしいと言われました。|@report の @format を とういつして ほしいと いわれました。
新しいpolicyについてmanagerから説明がありました。|あたらしい @policy について @manager から せつめいが ありました。
このstoreではcardしか使えません。|この @store では @card しか つかえません。
clientとのcallが長引いたので、lunchを食べ損ねました。|@client との @call が ながびいたので、 @lunch を たべそこねました。
来週はtrainingのために大阪へ行きます。|らいしゅうは @training の ために おおさかへ いきます。
このsoftwareはfreeで使えるのが便利です。|この @software は @free で つかえるのが べんりです。
sensorの値がおかしいので、deviceを再起動しました。|@sensor の あたいが おかしいので、 @device を さいきどうしました。
このgraphを見ると、trendがはっきり分かります。|この @graph を みると、 @trend が はっきり わかります。
partyの準備はcheckのlistを作って進めています。|@party の じゅんびは @check の @list を つくって すすめています。
このruleは分かりにくいので、例を足しましょう。|この @rule は わかりにくいので、 れいを たしましょう。
historyを見れば、誰がいつ変えたか分かります。|@history を みれば、 だれが いつ かえたか わかります。
templateを使えば、毎回同じ形で書けます。|@template を つかえば、 まいかい おなじ かたちで かけます。
このbuttonの位置が分かりにくいという声がありました。|この @button の いちが わかりにくいという こえが ありました。
projectorの調子が悪いので、別のroomを使いましょう。|@projector の ちょうしが わるいので、 べつの @room を つかいましょう。
今日のtopicは来年のstrategyについてです。|きょうの @topic は らいねんの @strategy についてです。
coffeeを飲みすぎて、夜に眠れませんでした。|@coffee を のみすぎて、 よるに ねむれませんでした。
このbrandのshoesは軽くて歩きやすいです。|この @brand の @shoes は かるくて あるきやすいです。
weatherのappで明日の雨を確認しました。|@weather の @app で あしたの あめを かくにんしました。
networkの設定はadminしか変えられません。|@network の せっていは @admin しか かえられません。
このchartは去年のdataをもとに作りました。|この @chart は きょねんの @data を もとに つくりました。
newsのsiteで新しいproductの記事を読みました。|@news の @site で あたらしい @product の きじを よみました。
このfolderに入っているfileは全部消して大丈夫です。|この @folder に はいっている @file は ぜんぶ けして だいじょうぶです。
contentの量が多すぎるので、sectionを分けました。|@content の りょうが おおすぎるので、 @section を わけました。
このmessageは自動で送られています。|この @message は じどうで おくられています。
bossに頼まれたtaskを夕方までに終わらせます。|@boss に たのまれた @task を ゆうがたまでに おわらせます。
"""

JAPANESE = """
明日は朝から会議があるので、早めに家を出るつもりです。|あしたは あさから かいぎが あるので、 はやめに いえを でる つもりです。
駅の近くに新しい本屋ができたので、週末に行ってみたいです。|えきの ちかくに あたらしい ほんやが できたので、 しゅうまつに いってみたいです。
雨が降りそうなので、傘を持って出かけたほうがいいですよ。|あめが ふりそうなので、 かさを もって でかけたほうが いいですよ。
この料理は思ったより簡単に作れるので、ぜひ試してみてください。|この りょうりは おもったより かんたんに つくれるので、 ぜひ ためして みてください。
去年の夏に行った旅行の写真を整理しています。|きょねんの なつに いった りょこうの しゃしんを せいりしています。
子供の頃に読んだ本をもう一度読み返しています。|こどもの ころに よんだ ほんを もういちど よみかえしています。|子どもの頃に読んだ本をもう一度読み返しています。
体調が悪いので、今日は早めに休ませてください。|たいちょうが わるいので、 きょうは はやめに やすませてください。
来月の引っ越しに向けて、少しずつ荷物をまとめています。|らいげつの ひっこしに むけて、 すこしずつ にもつを まとめています。
先週の土曜日に友達と映画を見に行きました。|せんしゅうの どようびに ともだちと えいがを みに いきました。
この道をまっすぐ行くと、右側に郵便局があります。|この みちを まっすぐ いくと、 みぎがわに ゆうびんきょくが あります。
お忙しいところ申し訳ありませんが、確認をお願いできますか。|おいそがしい ところ もうしわけ ありませんが、 かくにんを おねがい できますか。
毎日少しずつでも続けることが大切だと思います。|まいにち すこしずつでも つづけることが たいせつだと おもいます。
昨日は風が強くて、自転車に乗るのが大変でした。|きのうは かぜが つよくて、 じてんしゃに のるのが たいへんでした。
冷蔵庫の中に残っていた野菜でスープを作りました。|れいぞうこの なかに のこっていた やさいで すーぷを つくりました。
図書館で借りた本の返却日が明日までです。|としょかんで かりた ほんの へんきゃくびが あしたまでです。
新しい仕事にはだいぶ慣れてきましたが、まだ覚えることが多いです。|あたらしい しごとには だいぶ なれてきましたが、 まだ おぼえることが おおいです。
週末は天気が良ければ、山に登りに行く予定です。|しゅうまつは てんきが よければ、 やまに のぼりに いく よていです。|週末は天気がよければ、山に登りに行く予定です。
電車の中で寝てしまって、降りる駅を通り過ぎました。|でんしゃの なかで ねてしまって、 おりる えきを とおりすぎました。
久しぶりに実家に帰ったら、両親がとても喜んでくれました。|ひさしぶりに じっかに かえったら、 りょうしんが とても よろこんで くれました。
その件については、後ほど担当者から改めてご連絡します。|その けんに ついては、 のちほど たんとうしゃから あらためて ごれんらく します。
"""

ENGLISH = """
I will send you the report by Friday.|@i @will @send @you @the @report @by @friday.
Can we move the meeting to next week?|@can @we @move @the @meeting @to @next @week?
The build failed again, so I am looking into it.|@the @build @failed @again, @so @i @am @looking @into @it.
Thanks for the quick review.|@thanks @for @the @quick @review.
Please let me know if you have any questions.|@please @let @me @know @if @you @have @any @questions.
I think we should ship this after the tests pass.|@i @think @we @should @ship @this @after @the @tests @pass.
Sorry for the late reply, I was out of the office.|@sorry @for @the @late @reply, @i @was @out @of @the @office.
Let me check the logs and get back to you.|@let @me @check @the @logs @and @get @back @to @you.
The new version looks much faster than the old one.|@the @new @version @looks @much @faster @than @the @old @one.
Could you share the slides from yesterday?|@could @you @share @the @slides @from @yesterday?
We need to fix this before the release.|@we @need @to @fix @this @before @the @release.
I am going to be a little late today.|@i @am @going @to @be @a @little @late @today.
Do you have time for a quick call this afternoon?|@do @you @have @time @for @a @quick @call @this @afternoon?
The server is down, but we are working on it.|@the @server @is @down, @but @we @are @working @on @it.
Have a nice weekend and see you on Monday.|@have @a @nice @weekend @and @see @you @on @monday.
I just pushed the changes to the main branch.|@i @just @pushed @the @changes @to @the @main @branch.
That sounds good to me.|@that @sounds @good @to @me.
The client wants the design to be simpler.|@the @client @wants @the @design @to @be @simpler.
I will be working from home tomorrow.|@i @will @be @working @from @home @tomorrow.
We can talk about it at lunch.|@we @can @talk @about @it @at @lunch.
"""


VARIANTS = [("分か", "わか"), ("作るとき", "作る時"), ("溜まった", "たまった"), ("売り上げ", "売上"),
            ("できた", "出来た"), ("上げたら", "あげたら"), ("良ければ", "よければ"), ("おすすめ", "お勧め")]


def rows(block: str):
    for line in block.strip().splitlines():
        parts = line.split("|")
        text, chunks, also = parts[0], parts[1].split(), parts[2:]
        # A named word with a capital (Google) is right in lower case too,
        # except where it starts the sentence.
        lowered = text
        for word in re.findall(r"(?<=.)\b[A-Z][a-z]+\b", text):
            if not text.startswith(word):
                lowered = re.sub(rf"\b{word}\b", word.lower(), lowered)
        others = [a if len(a) > 8 else text.replace(*find_swap(text, a)) for a in also]
        # Words Japanese writes either way (分かる, わかる): each way is right.
        for written, other in VARIANTS:
            for base in [text] + list(others):
                if written in base and base.replace(written, other) not in others:
                    others.append(base.replace(written, other))
        if lowered != text and text[0] not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
            others.append(lowered)
        yield {"text": text, "chunks": chunks, "also": others}


def find_swap(text: str, short: str):
    """A short alternative (20代) stands for the part of the text it replaces."""
    for kanji, other in (("二十代", "20代"), ("一台", "1台"), ("売り上げ", "売上げ"), ("おすすめです", "お勧めです")):
        if other == short:
            return kanji, other
    raise ValueError(short)


def main():
    out = Path(sys.argv[1])
    for name, block in (("mix_long", MIXED), ("mix_long_ja", JAPANESE), ("mix_long_en", ENGLISH)):
        lines = [json.dumps(row, ensure_ascii=False) for row in rows(block)]
        (out / f"{name}.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(name, len(lines))


if __name__ == "__main__":
    main()
