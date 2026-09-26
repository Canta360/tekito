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

export function hostRequest(type, payload = {}) {
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
      finish({ ok: false, error: "TEKITO Settings host is unavailable.", state: null });
      return;
    }
    webview().addEventListener("message", onMessage);
    webview().postMessage(JSON.stringify({ type, requestId, ...payload }));
    // File dialogs (import/export) keep the host busy until the user closes them.
    const opensDialog = ["dictionary.import", "dictionary.export", "excludedApps.browse"].includes(type);
    const timeout = opensDialog ? 600000 : 4000;
    window.setTimeout(() => finish({ ok: false, error: "The settings host did not respond.", state: null }), timeout);
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
