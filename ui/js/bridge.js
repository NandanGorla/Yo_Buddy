// Connects the UI to the C++ host through WebView2 web messages.
// This is the only way out of the page: the UI makes no network requests of its own.

import { buildMessage, validateHostMessage } from './protocol.js';

const webview = window.chrome && window.chrome.webview ? window.chrome.webview : null;
const handlers = new Map();

// False when the page is opened in an ordinary browser, where there is no host to talk to.
export const hasHost = webview !== null;

// Registers the function that handles one host message type.
export function on(type, handler) {
  handlers.set(type, handler);
}

// Sends one message to the host. Throws if the message would break the protocol.
export function send(type, payload = {}) {
  const message = buildMessage(type, payload);
  if (webview) webview.postMessage(message);
}

// Writes a line to the host's local log file. Never throws: logging must not break the UI.
export function logToHost(level, text) {
  try {
    send('ui.log', { level, message: String(text).slice(0, 500) });
  } catch {
    // Nothing useful can be done if logging itself fails.
  }
}

export function start() {
  if (!webview) return;
  webview.addEventListener('message', (event) => {
    const result = validateHostMessage(event.data);
    if (!result.ok) {
      logToHost('error', `rejected host message: ${result.error}`);
      return;
    }
    const handler = handlers.get(result.type);
    if (handler) handler(result.payload);
  });
}
