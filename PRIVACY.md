# Privacy

TEKITO works entirely on your PC. It does not send what you type, your
dictionary or what it has learned anywhere. It has no account, no telemetry
and no update check, and nothing in the input path uses the network. The
only downloads are the ones you ask for: the Japanese data, the postal code
data, the AI models, and what runs them on a GPU or an NPU.

## What TEKITO keeps, and where

| Location | What is in it |
| --- | --- |
| `%LOCALAPPDATA%\TEKITO\user.db` (and `user.db-wal`, `user.db-shm` beside it) | Your settings, your dictionary, what TEKITO has learned from your choices, and your last 30 Japanese conversions while that is on |
| `%LOCALAPPDATA%\TEKITO\user.key` | Only when you turn on **Encrypt your dictionary and learning**: the key that encrypts `user.db`, protected by Windows for your account and by your recovery code. The recovery code itself is not kept |
| `%LOCALAPPDATA%\TEKITO\data` | The language data packs, and the AI models when you download them |
| `%LOCALAPPDATA%\TEKITO\settings-webview` | The Settings window's browser cache, and which pages you folded or opened |
| `HKEY_CURRENT_USER\Software\TEKITO\Companion`, and `TEKITO Companion` under `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run` | Only when you turn on **Desktop companion**: where you put it on the desktop, and that it starts when you sign in |
| `%TEMP%\TekitoTsf.log` | A diagnostic log, written only by trace builds or when `TEKITO_TSF_TRACE=1` is set |

What TEKITO learns is a count of which suggestions you pick or turn down for
a word, and in Japanese, which conversion you pick for a reading. It does not
store a history of what you typed, except the last 30 Japanese conversions
(the reading and what it became, never the keys pressed and none from a
private window), kept so that you can report a wrong one; Settings → About →
**Report wrong conversions** turns this off and deletes them. To read a sentence as a
whole, Japanese conversion keeps the last few words you committed in memory
only, while you go on typing right after them; they are dropped once the
caret moves elsewhere and are never saved. The text you just entered is also
kept in memory so that Ctrl+Backspace can take it back, until you type on.
Reconversion (Henkan on selected text) reads the selected text on your
computer only, and importing words from another IME reads the file you
choose on your computer only; neither sends anything. The
diagnostic log and **Copy diagnostics** (About) in Settings record events, timings
and error codes, never text.

Unless you turn on **Encrypt your dictionary and learning** (Settings →
Dictionary & learning), the database is not encrypted; treat it like any
other private file in your profile.

So that TEKITO also works in the Start menu's search, Settings and Store
apps, the setup program lets those sandboxed apps read the program folder
(`%LOCALAPPDATA%\Programs\TEKITO`) and the language data
(`%LOCALAPPDATA%\TEKITO\data`). They get read access only, and not to
`user.db`: your dictionary and what TEKITO has learned stay out of their
reach, and there TEKITO types with the default settings.

## Your controls

- **Settings → Dictionary & learning → Learn from chosen candidates** turns
  learning on or off, and **Delete all** clears what TEKITO has learned.
- **Secret mode** (the TEKITO button on the taskbar, or **Settings →
  Dictionary & learning**): until you turn it off, nothing you type is
  learned or kept, in every app. It turns off when you sign out or restart.
- **Settings → About → Report wrong conversions** turns the list of recent
  conversions on or off. **Report** opens a GitHub issue form in your browser
  with the reading and the result filled in; nothing is sent until you submit
  it there. **Report a problem** fills in the version and settings the same
  way.
- **Settings → General → Apps where TEKITO is off** makes TEKITO pass
  every key through in the apps you list. It also stays off in terminals,
  apps running as administrator, and password, PIN, email, URL and number
  fields.
- Uninstalling from Installed apps removes TEKITO with its data, dictionary,
  learning and settings. It can export your dictionary to Documents first.

## Other software

Windows, the Microsoft Edge WebView2 Runtime that draws the Settings window,
and the apps you type in have their own privacy terms. If the WebView2
Runtime is missing, the setup program offers to open Microsoft's download
page; it never downloads anything on its own.

## The Japanese data download

When you choose Japanese in the setup program, or add it later in Settings,
TEKITO downloads the Japanese data from TEKITO's release on GitHub
(github.com) and checks it against the checksum it carries. With **Check with AI** chosen too, it also downloads the AI model (see
below). These requests are the setup program's only network use; they send nothing
about you beyond what any download does, such as your IP address, and
GitHub's privacy terms apply to them. Installing from a ZIP you downloaded
yourself (`install.ps1 -JapaneseDataPath`) makes no request at all.

## AI models

The AI models are optional. Choosing **Check with AI** in the setup program,
or a model under **Settings → Conversion → Check with AI** (which asks
first), downloads the model, Settings' own smaller model for Home and what
runs them from TEKITO's release on GitHub
(github.com) and checks them against the checksums Settings carries. The
request sends nothing about you beyond what any download does, such as your
IP address, and GitHub's privacy terms apply to it. The model then runs on
your PC, on the CPU, the GPU or the NPU, in a separate TEKITO process; what
you type is passed to it on your PC only and is not saved. What you write
in Home's box in Settings is read the same way by Settings' model, on your
PC, and is not saved either. **Remove the model** deletes the model.

Choosing **GPU** or **NPU** under **Settings → Conversion → Run AI on**
(which asks first)
downloads Microsoft's Windows ML from TEKITO's release on GitHub. With
**NPU**, Windows ML may also download the part that runs your NPU from
Microsoft, and Microsoft's privacy statement applies to that download.
TEKITO turns off the telemetry of ONNX Runtime and Windows ML. The first
time, the model is prepared for the NPU and kept with the model (1 to 2
GB); **Remove the model** deletes it too.

## Postal codes

Turning on **Postal codes to addresses** in Settings asks first, then
downloads Japan Post's postal code file (utf_ken_all.zip): the copy on
TEKITO's releases on GitHub (github.com), updated monthly, or Japan Post's
own (www.post.japanpost.jp) when that copy cannot be had. The request sends
nothing about you beyond what any download does, such as your IP address,
and GitHub's or Japan Post's privacy terms apply to it. **Use a file already downloaded**
reads a file you choose on your computer and makes no request.
