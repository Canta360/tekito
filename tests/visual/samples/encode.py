#!/usr/bin/env python3
"""Records and encodes the sample clips shown in Settings (入力の見本).

    python tests/visual/samples/encode.py <testbed.exe> <out folder> [script ...]

Each script (tests/visual/samples/*.txt, all of them by default) is typed
into tekito_testbed --record, once with the light and once with the dark
candidate list, and the frames become <name>-light.mp4 and <name>-dark.mp4
in the out folder, encoded by headless Chrome (encode.html). A small page
window and the candidate list appear at the top left of the screen while
recording; they never take the focus. Only the standard library is used.
"""

from __future__ import annotations

import functools
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHROME = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")


class Handler(http.server.SimpleHTTPRequestHandler):
    frames: Path
    target: Path
    done: threading.Event
    ok: list[bool]

    def translate_path(self, path: str) -> str:
        if path.startswith("/frames/"):
            return str(self.frames / path.split("?")[0][len("/frames/"):])
        return super().translate_path(path)

    def do_POST(self) -> None:
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/save":
            self.target.write_bytes(body)
        elif self.path == "/log":
            print(f"  {self.target.name}: {body.decode('utf-8', 'replace')}")
        elif self.path == "/done":
            self.ok.append(body == b"ok")
            self.done.set()
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args) -> None:
        pass


def encode(frames: Path, target: Path) -> bool:
    done = threading.Event()
    ok: list[bool] = []
    handler = functools.partial(
        type("Bound", (Handler,), {"frames": frames, "target": target, "done": done, "ok": ok}), directory=str(HERE))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="tekito-clips-")
    chrome = subprocess.Popen([
        str(CHROME), "--headless=new", f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
        "--disable-extensions", f"http://127.0.0.1:{server.server_address[1]}/encode.html",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        finished = done.wait(timeout=300)
    finally:
        chrome.terminate()
        chrome.wait(timeout=30)
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    return finished and all(ok)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    testbed, out = Path(sys.argv[1]), Path(sys.argv[2])
    scripts = [Path(p) for p in sys.argv[3:]] or sorted(HERE.glob("*.txt"))
    out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="tekito-frames-"))
    failed = 0
    try:
        for script in scripts:
            for theme in ("light", "dark"):
                frames = work / f"{script.stem}-{theme}"
                environment = dict(os.environ, TEKITO_PREVIEW_THEME=theme, TEKITO_PREVIEW_LANGUAGE="ja")
                result = subprocess.run([str(testbed), "--defaults", "--record", str(script), str(frames)], env=environment)
                target = out / f"{script.stem}-{theme}.mp4"
                if result.returncode != 0 or not encode(frames, target):
                    print(f"{target.name}: failed")
                    failed += 1
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
