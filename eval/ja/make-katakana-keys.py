"""Writes eval/ja/katakana_keys.tsv: loanwords and names typed as their
katakana reads, where the English should be among the choices
(`tekito_ja_eval --keys-corpus ... --loanwords ...` reports it as in_list).

Expected texts as in make-mixed-keys.py: {Google|グーグル} is either.
"""

import importlib.util
import pathlib

here = pathlib.Path(__file__).parent
spec = importlib.util.spec_from_file_location("mixed", here / "make-mixed-keys.py")
mixed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mixed)

# Keys, then the text with the English word (the katakana is fine first;
# the English must be in the list).
ROWS = [
    ("gu-gurude", "Googleで"),
    ("gittohabuni", "GitHubに"),
    ("dokka-wo", "Dockerを"),
    ("no-shonnni", "Notionに"),
    ("yu-chu-bude", "YouTubeで"),
    ("tuitta-", "Twitter"),
    ("dhisuko-dode", "Discordで"),
    ("supothifaide", "Spotifyで"),
    ("amazonnde", "Amazonで"),
    ("paisonnde", "Pythonで"),
    ("kuro-muga", "Chromeが"),
    ("surakkude", "Slackで"),
    ("zu-mude", "Zoomで"),
    ("ekuserunode-ta", "Excelのデータ"),
    ("wa-dode", "Wordで"),
    ("pawa-pointo", "PowerPoint"),
    ("insutaguramuni", "Instagramに"),
    ("feisubukku", "Facebook"),
    ("nettofurikkusude", "Netflixで"),
    ("maikurosofuto", "Microsoft"),
    ("appuru", "Apple"),
    ("tesurano", "Teslaの"),
    ("toyotano", "Toyotaの"),
    ("soni-", "Sony"),
    ("nintendou", "Nintendo"),
    ("mi-thinngu", "meeting"),
    ("purezennte-shonn", "presentation"),
    ("deddorainnga", "deadlineが"),
    ("sukejuuruwo", "scheduleを"),
    ("tasukuwo", "taskを"),
    ("purojekutono", "projectの"),
    ("apude-towo", "updateを"),
    ("daunro-dosuru", "downloadする"),
    ("roguinndekinai", "loginできない"),
    ("pasuwa-dowo", "passwordを"),
    ("akaunntowo", "accountを"),
    ("sa-ba-ga", "serverが"),
    ("de-tabe-su", "database"),
    ("inta-netto", "internet"),
    ("konpyu-ta-", "computer"),
    ("suma-tohonn", "smartphone"),
    ("rinkuwo", "linkを"),
    ("fairuwo", "fileを"),
    ("forudani", "folderに"),
    ("bakkuappu", "backup"),
    ("sekyurithi", "security"),
    ("ne-ttowa-ku", "network"),
    ("arugorizumu", "algorithm"),
    ("intafe-su", "interface"),
    ("kurauddo", "cloud"),
]


def main() -> None:
    lines = [f"katakana\t{n}\t{keys}\t{text}" for n, (keys, text) in enumerate(ROWS, 1)]
    out = here / "katakana_keys.tsv"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(lines)} rows -> {out}")


if __name__ == "__main__":
    main()
