# Compatibility

TEKITO works through TSF, so any app that supports Windows input methods can
use it. Apps differ in the details: where they report the caret, whether they
accept edits synchronously, which fields they mark as passwords or URLs. When
TEKITO cannot own a composition in a field, it lets the keys through
untouched and shows nothing.

| App | Status |
| --- | --- |
| Notepad | Works |
| Word, Outlook | Not tested yet |
| Edge, Chrome, Firefox | Not tested yet |
| Visual Studio Code and other Electron apps | Not tested yet |
| Teams, Slack, Discord | Not tested yet |
| WPF, WinForms, WinUI 3, UWP fields | Not tested yet |
| Start menu search, Settings app | Not supported yet (needs immersive-mode support) |
| Windows Terminal, PowerShell, Command Prompt | Stays off |
| Apps running as administrator | Stays off |
| Apps listed in Settings | Stays off |

## What to check in an app

- TEKITO can be selected with Win+Space, and the switch key (Hankaku/Zenkaku
  on a Japanese keyboard, Alt+` on a US one) goes through the modes.
- In Japanese, the underlined kana converts on Space, the list opens under
  the phrase in focus and follows it as the focus moves, and Enter commits
  without reaching the app.
- The first letter underlines the word and shows the list under it.
- The list follows the word when the window scrolls or moves, stays on
  screen near the edges, and scales with the display.
- Clicking elsewhere, switching windows or moving the caret with the mouse
  commits the word as typed and closes the list.
- Space, Shift+Space, Tab and the arrow keys move through the list; clicking a
  choice picks it without taking focus from the app.
- Punctuation and digits end the word. Enter ends the word and then does what
  the app does with Enter (a new line, or sending a chat message).
- Password, PIN, URL, email and number fields get no list.
- The log contains no typed text.

## Collecting a log

Use a build whose DLL name ends in `Trace` (see `TEKITO_TSF_OUTPUT_NAME`), or
set `TEKITO_TSF_TRACE=1` for the app, then:

```powershell
Get-Content "$env:TEMP\TekitoTsf.log" -Tail 120
```

The log holds event names, timings, process ids and error codes, never text.
