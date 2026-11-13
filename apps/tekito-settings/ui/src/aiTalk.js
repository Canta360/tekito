// Home's AI beyond settings: small talk, and what people ask about TEKITO
// itself. Each is found by its words, before the settings are scored, and
// answered with one of its lines. `short`: only a short request (a word or
// two), so that the words inside a request about a setting ("Ctrl+Space が
// 使えない") are left to the settings. `show`: a setting or page shown with
// the answer.

const talks = [
  // Small talk.
  {
    id: "morning", short: true, pattern: /おはよ|good morning/i,
    ja: ["おはようございます。今日もよろしくお願いします。", "おはようございます。今日の入力もおまかせください。"],
    en: ["Good morning. Ready when you are.", "Good morning. Let's get typing."],
  },
  {
    id: "evening", short: true, pattern: /こんばん|good evening/i,
    ja: ["こんばんは。遅くまでおつかれさまです。", "こんばんは。夜の入力もおまかせください。"],
    en: ["Good evening. Working late?", "Good evening. Still here if you need me."],
  },
  {
    id: "nice", short: true, pattern: /はじめまして|初めまして|nice to meet you/i,
    ja: ["はじめまして。TEKITO の設定係です。困っていることを聞かせてもらえれば、合う設定を探します。"],
    en: ["Nice to meet you. I look after TEKITO's settings: tell me what bothers you and I'll find the one that fits."],
  },
  {
    id: "hello", short: true, pattern: /こんにち|やあ|ハロー|^(hello|hi|hey|howdy)\b/i,
    ja: ["こんにちは。設定のことで困っていることがあれば、そのまま聞いてください。", "こんにちは。合う設定を探すのが得意です。"],
    en: ["Hello. If something about typing bothers you, just tell me.", "Hi. Finding the right setting is what I do best."],
  },
  {
    id: "thanks", short: true, pattern: /ありがと|助か|サンキュ|感謝|thank|thx|ty\b/i,
    ja: ["どういたしまして。", "お役に立ててよかったです。", "いつでもどうぞ。"],
    en: ["You're welcome.", "Glad that helped.", "Any time."],
  },
  {
    id: "cute", short: true, pattern: /^(かわい|可愛)|^(so )?(cute|adorable)/i,
    ja: ["ありがとうございます。デスクトップにも出られますよ。", "照れますね。デスクトップの隅にも置けます。"],
    en: ["Thank you. I can sit on your desktop too.", "Aw. You can keep me in a corner of the desktop, too."],
    show: { type: "setting", id: "companion" },
  },
  {
    id: "praise", short: true, pattern: /^(すご|すげ|えらい|天才|賢い|かしこい|最高|いいね|やるね|ナイス)|^(you('re| are) )?(so )?(great|awesome|amazing|smart|nice|brilliant)|^(good job|well done)/i,
    ja: ["ありがとうございます。そう言ってもらえるとうれしいです。", "照れますね。これからもがんばります。"],
    en: ["Thank you. That's nice to hear.", "Thanks. I'll keep at it."],
  },
  {
    id: "love", short: true, pattern: /好きです|大好き|愛して|結婚|付き合|love you|marry/i,
    ja: ["気持ちだけ受け取っておきます。", "ありがとうございます。入力のほうでお返しします。"],
    en: ["I'll take that as a compliment.", "Thank you. I'll show it by fixing your typos."],
  },
  {
    id: "who", short: true,
    pattern: /(あなた|君|きみ|お前)(は|って|の)?(誰|だれ|何者|名前|なに|何)|^(誰|だれ|何者|名前は|お名前)|who are you|your name|what are you/i,
    ja: ["TEKITO の設定係です。困っていることから、合う設定を探します。", "TEKITO の中で、設定を探したり変えたりしています。"],
    en: ["I look after TEKITO's settings. Tell me what bothers you and I'll find the one that fits.",
      "I'm the one inside TEKITO who finds and changes settings."],
  },
  {
    id: "howAreYou", short: true, pattern: /元気|調子|how are you|how's it going/i,
    ja: ["元気です。キーの音を聞くと元気が出ます。", "元気です。入力ミスを直すのが楽しみです。"],
    en: ["Doing well. The sound of keys keeps me going.", "Good, thanks. Ready to fix some typos."],
  },
  {
    id: "tired", short: true, pattern: /疲れ|つかれ|眠|ねむ|しんど|だる|tired(?! of)|sleepy|exhausted/i,
    ja: ["おつかれさまです。少し休みましょう。", "無理しないでください。入力ミスはこちらで直しておきます。"],
    en: ["You've earned a break.", "Take it easy. I'll keep fixing the typos."],
  },
  {
    id: "hungry", short: true, pattern: /お腹|おなか|腹減|はらへ|腹ペコ|はらぺこ|hungry/i,
    ja: ["何か食べてきてください。その間、キーボードは見ておきます。", "ごはんにしましょう。続きはそれからでも大丈夫です。"],
    en: ["Go grab something. I'll watch the keyboard.", "Time for a snack. The typing can wait."],
  },
  {
    id: "bored", short: true, pattern: /暇|ひま|退屈|つまらな|つまんな|bored/i,
    ja: ["設定を見て回ってみるのはどうでしょう。思わぬ発見があるかもしれません。", "それなら、いろいろ聞いてみてください。隠れた機能があるかもしれません。"],
    en: ["Why not look around Settings? You might find something unexpected.", "Then ask me things. There may be a hidden feature or two."],
  },
  {
    id: "joke", short: true, pattern: /面白|おもしろ|ジョーク|冗談|笑わせ|ネタ|joke|funny|make me laugh/i,
    ja: ["早口言葉は苦手です。「きしゃのきしゃがきしゃできしゃした」は、まだうまく変換できません。",
      "名前は TEKITO ですが、変換はまじめにやっています。", "Space キーは、いちばんたくさん押されるキーです。たぶん。"],
    en: ["My name means “whatever” in Japanese, but I take typos very seriously.",
      "Space is the most pressed key on the keyboard. Probably.", "Teh best typo is the one you never see."],
  },
  {
    id: "outside", short: true, pattern: /天気|何時|今の時間|ニュース|株価|weather|what time|news/i,
    ja: ["インターネットにはつながっていないので、それはわかりません。日付なら「きょう」から変換できます。",
      "ごめんなさい、それはわかりません。この PC の中だけで動いているので。"],
    en: ["I don't go online, so I can't tell. I can turn きょう into today's date, though.",
      "Sorry, I can't know that. I only run on this PC."],
  },
  {
    id: "likes", short: true, pattern: /好きな(食べ物|もの|こと|色)|好物|趣味|favou?rite (food|thing|color|colour)|hobby/i,
    ja: ["きれいに変換された文が好きです。", "入力ミスが一発で直った瞬間が好きです。"],
    en: ["A sentence that comes out right the first time.", "The moment a typo disappears on Space."],
  },
  {
    id: "night", short: true, pattern: /おやすみ|good night|goodnight/i,
    ja: ["おやすみなさい。また明日。", "おやすみなさい。ゆっくり休んでください。"],
    en: ["Good night. See you tomorrow.", "Good night. Rest well."],
  },
  {
    id: "rude", short: true, pattern: /^(ばか|バカ|馬鹿|アホ|あほ|ポンコツ)|使えないな|使えねー|役立たず|役に立たない(な|ね)|^(you.re )?(stupid|useless|dumb)/i,
    ja: ["すみません、まだ勉強中です。困っていることをもう少し詳しく聞かせてもらえれば、探し直します。",
      "ごめんなさい。どこがうまくいかないか教えてもらえれば、合う設定を探します。"],
    en: ["Sorry, still learning. Tell me a little more about what's wrong and I'll look again.",
      "Sorry about that. Tell me what isn't working and I'll find the setting for it."],
  },
  // About TEKITO.
  {
    id: "about", pattern: /TEKITO.*(何|なに|とは)|このアプリ(は|って)|what is tekito|what's tekito|what does tekito/i,
    ja: ["TEKITO は、英語と日本語を 1 つで入力できる IME です。英語の入力ミスの修正、文全体を読んだ日本語の変換、日本語と英語のまぜ打ちができます。"],
    en: ["TEKITO is one input method for English and Japanese: English typos fixed as you type, Japanese converted reading the whole sentence, and both mixed in one line."],
  },
  {
    id: "privacy", pattern: /(入力|内容|データ|文字|情報).{0,8}(送信|送られ|送って|外部|漏れ|集め)|プライバシ|個人情報|サーバ|privacy|upload|server|(send|sent|collect).{0,12}(data|what i type|text)/i,
    ja: ["入力した内容が外部に送られることはありません。変換も AI も、すべてこの PC の中で動いています。"],
    en: ["Nothing you type leaves your PC. Conversion and AI both run right here."],
  },
  {
    id: "price", pattern: /無料|有料|タダ|ただで|料金|値段|いくら(かかる|ですか|なの)|お金|課金|\bfree\b|\bprice|\bcosts?\b|\bpay\b/i,
    ja: ["料金やライセンスは、TEKITO の配布ページで案内しています。"],
    en: ["Pricing and licensing are on TEKITO's download page."],
  },
  {
    id: "platform", pattern: /mac|linux|iphone|ipad|android|スマホ|携帯|windows以外|ウィンドウズ以外|他の\s*os|other (os|platforms)/i,
    ja: ["今のところ Windows 専用です。"],
    en: ["For now, TEKITO is for Windows only."],
  },
  {
    id: "update", pattern: /アップデート|バージョンアップ|新しいバージョン|最新(版|の|バージョン)|\bupdate|upgrade|new version/i,
    ja: ["TEKITO は自分から更新を確認しません。新しいバージョンは、GitHub のリリースページから入手できます。"],
    en: ["TEKITO never checks for updates by itself. New versions are on its GitHub releases page."],
  },
  {
    id: "uninstall", pattern: /アンインストール|uninstall|(TEKITO|これ)を(消|削除)/i,
    ja: ["Windows の「設定」→「アプリ」→「インストールされているアプリ」から削除できます。削除するときに、ユーザー辞書をドキュメントに書き出せます。"],
    en: ["Remove it from Windows Settings → Apps → Installed apps. You can export your dictionary to Documents on the way."],
  },
  {
    id: "typeJapanese", pattern: /日本語.{0,4}(入力|打|書).{0,6}(には|方法|方|どう|やり方)|日本語.{0,8}(どう|方法|やり方).{0,6}(入力|打|書)|how.*(type|write).*japanese/i,
    ja: ["日本語のモードでローマ字を打つと、ひらがなで表示され、Space で変換されます。モードは、日本語キーボードなら半角/全角、US キーボードなら Alt+` で切り替えられます。"],
    en: ["In Japanese mode, romaji shows as hiragana and Space converts it. Switch modes with Half-width/Full-width on a Japanese keyboard, or Alt+` on a US keyboard."],
    show: { type: "setting", id: "switchKey" },
  },
  {
    id: "ai", pattern: /AI(は|って|が|の)(何|なに|役|仕事)|what does (the )?ai/i,
    ja: ["変換の結果を AI が見直して、文として自然なものを選びます。この画面の設定探しにも AI を使っています。どちらもこの PC の中で動いています。"],
    en: ["AI double-checks Japanese conversions and picks what reads naturally, and it finds settings here. Both run on your PC."],
    show: { type: "setting", id: "aiModel" },
  },
  {
    id: "mix", pattern: /(まぜ打ち|混ぜ打ち)(って|とは|は何)|what is mix/i,
    ja: ["まぜ打ちは、日本語と英語を切り替えずに入力できるモードです。Space で区切るごとに、日本語か英語かが判断されます。"],
    en: ["Mix types Japanese and English without switching: each part you end with Space is taken as Japanese or English."],
    show: { type: "setting", id: "switchOrder" },
  },
];

// The longest a request may be and still count as small talk, in letters:
// English takes more of them for the same words.
const kShort = { ja: 14, en: 32 };

// What was asked, without the marks around it.
const plain = (question) => question.replace(/[\s、。，．！？!?…ー〜~]+$/u, "").trim();

// The talk `question` is, as { id, reply, show } in `lang`, or nothing.
export function talk(question, lang) {
  const text = plain(question);
  const found = talks.find((one) => one.pattern.test(text) && (!one.short || text.length <= (kShort[lang] || kShort.en)));
  if (!found) return null;
  const lines = found[lang] || found.en;
  return { id: found.id, reply: lines[Math.floor(Math.random() * lines.length)], show: found.show ? [found.show] : [] };
}

// Asking what a thing is, not to change it (シークレットモードって何).
export const asksWhat = (question) => /って何|ってなに|とは|って\?|って？|は何|はなに|what is|what's|what does/i.test(question);

// One of `lines` (or the one line).
export const pick = (lines) => (Array.isArray(lines) ? lines[Math.floor(Math.random() * lines.length)] : lines);
