// Bridge to the native Settings host (apps/tekito-settings/main.cpp) over
// WebView2 web messages.
//
// Requests carry a requestId and get exactly one reply with the full state.
// The host also pushes {"type":"state.changed","state":...} on its own when
// something changes outside this page (e.g. the mode is switched from the
// Language Bar), so the page never has to poll.

function webview() {
  return window.chrome?.webview;
}

function parse(data) {
  if (typeof data !== "string") return data;
  try {
    return JSON.parse(data);
  } catch {
    return null;
  }
}

// `files` (File objects, e.g. dropped on the page) reach the host with the
// message, which sees their paths.
export function hostRequest(type, payload = {}, files = []) {
  return new Promise((resolve) => {
    const requestId = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    let settled = false;
    const finish = (value) => {
      if (settled) return;
      settled = true;
      webview()?.removeEventListener("message", onMessage);
      resolve(value);
    };
    const onMessage = (event) => {
      const message = parse(event.data);
      if (message?.requestId !== requestId) return;
      const response = message.payload || message;
      finish({
        ok: response?.ok === true,
        error: response?.error || "",
        state: response?.state || null,
        text: response?.text || "",
      });
    };
    if (!webview()) {
      finish({ ok: false, error: "", state: null });  // the page shows its own message
      return;
    }
    webview().addEventListener("message", onMessage);
    const text = JSON.stringify({ type, requestId, ...payload });
    if (files.length && webview().postMessageWithAdditionalObjects) {
      webview().postMessageWithAdditionalObjects(text, files);
    } else {
      webview().postMessage(text);
    }
    // File dialogs (import/export) keep the host busy until the user closes
    // them; adding postal codes takes a few seconds.
    const opensDialog = ["dictionary.import", "dictionary.export", "excludedApps.browse",
      "postalCodes.browse", "postalCodes.import", "japaneseWords.browse", "japaneseWords.import"].includes(type);
    const timeout = opensDialog ? 600000 : 4000;
    window.setTimeout(() => finish({ ok: false, error: "", state: null }), timeout);
  });
}

export function onHostStateChanged(callback) {
  const onMessage = (event) => {
    const message = parse(event.data);
    if (message?.type === "state.changed" && message.state) callback(message.state);
  };
  webview()?.addEventListener("message", onMessage);
  return () => webview()?.removeEventListener("message", onMessage);
}
