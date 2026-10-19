// Copies the third-party notices and license texts into public/notices, so
// the built page carries them and Settings → About shows them on this PC.
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..", "..");
const target = join(here, "public", "notices");
const files = {
  "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
  "material-symbols-LICENSE.txt": "third_party/material-symbols/LICENSE",
  "OFL.txt": "assets/fonts/OFL.txt",
  "webview2-LICENSE.txt": "third_party/webview2/LICENSE.txt",
  "webview2-NOTICE.txt": "third_party/webview2/NOTICE.txt",
};
mkdirSync(target, { recursive: true });
for (const [name, source] of Object.entries(files)) copyFileSync(join(root, source), join(target, name));
