// The bridge protocol, JavaScript side.
//
// Every message in both directions is { v: 1, type: "...", payload: { ... } }.
// The rules here mirror src/core/protocol.h: change the two files together.
//
// Outgoing messages are checked before they are posted, so a UI bug is caught here and not by
// a rejection from the host. Incoming messages are checked before any UI code sees them, so the
// rest of the UI can trust their shape.

export const PROTOCOL_VERSION = 1;

const MAX_CHAT_TEXT_BYTES = 200 * 1000;
const MAX_LOG_BYTES = 2000;
const MAX_BASE_URL = 2048;
const MAX_TEXT = 1024 * 1024;

const ID_RE = /^[a-z0-9_-]{1,64}$/;
const MODEL_RE = /^[A-Za-z0-9._:/@-]{0,128}$/;
const API_KEY_RE = /^[\x21-\x7E]{1,512}$/;

const encoder = new TextEncoder();
const byteLength = (s) => encoder.encode(s).length;

const isObject = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);
const isString = (v, max = MAX_TEXT) => typeof v === 'string' && v.length <= max;
const isId = (v) => typeof v === 'string' && ID_RE.test(v);
const isBool = (v) => typeof v === 'boolean';
const isCountOrNull = (v) => v === null || (Number.isSafeInteger(v) && v >= 0);

// True when `obj` has exactly these keys: nothing missing and nothing extra.
function hasExactly(obj, keys) {
  if (!isObject(obj)) return false;
  const own = Object.keys(obj);
  return own.length === keys.length && keys.every((k) => Object.prototype.hasOwnProperty.call(obj, k));
}

// ---- UI -> host ----

const OUTGOING = {
  'app.ready': (p) => hasExactly(p, []),
  'chat.send': (p) =>
    hasExactly(p, ['request_id', 'text']) &&
    isId(p.request_id) &&
    typeof p.text === 'string' &&
    p.text.length > 0 &&
    byteLength(p.text) <= MAX_CHAT_TEXT_BYTES,
  'chat.cancel': (p) => hasExactly(p, ['request_id']) && isId(p.request_id),
  'chat.clear': (p) => hasExactly(p, []),
  'settings.set_theme': (p) => hasExactly(p, ['theme']) && isId(p.theme),
  'settings.set_provider': (p) =>
    hasExactly(p, ['provider', 'base_url', 'model']) &&
    isId(p.provider) &&
    isString(p.base_url, MAX_BASE_URL) &&
    p.base_url.length > 0 &&
    typeof p.model === 'string' &&
    MODEL_RE.test(p.model),
  'settings.set_api_key': (p) =>
    hasExactly(p, ['provider', 'key']) && isId(p.provider) && typeof p.key === 'string' && API_KEY_RE.test(p.key),
  'settings.clear_api_key': (p) => hasExactly(p, ['provider']) && isId(p.provider),
  'settings.set_hotkey': (p) => hasExactly(p, ['hotkey']) && isString(p.hotkey, 64) && p.hotkey.length > 0,
  'settings.set_panel': (p) =>
    hasExactly(p, ['hide_on_click_away', 'pinned']) && isBool(p.hide_on_click_away) && isBool(p.pinned),
  'window.hide': (p) => hasExactly(p, []),
  'window.open_main': (p) => hasExactly(p, ['settings']) && isBool(p.settings),
  'ui.log': (p) =>
    hasExactly(p, ['level', 'message']) &&
    ['info', 'warn', 'error'].includes(p.level) &&
    typeof p.message === 'string' &&
    byteLength(p.message) <= MAX_LOG_BYTES,
};

// Builds a message for the host. Throws when the type is unknown or the payload is malformed.
export function buildMessage(type, payload = {}) {
  const check = OUTGOING[type];
  if (!check) throw new Error(`unknown outgoing message type: ${type}`);
  if (!check(payload)) throw new Error(`invalid payload for ${type}`);
  return { v: PROTOCOL_VERSION, type, payload };
}

// ---- host -> UI ----

const isTokenMap = (v) => isObject(v) && Object.values(v).every((x) => isString(x, 256));

const isTheme = (t) =>
  hasExactly(t, ['id', 'name', 'kind', 'tokens', 'overlay']) &&
  isId(t.id) &&
  isString(t.name, 64) &&
  ['light', 'dark', 'high-contrast'].includes(t.kind) &&
  isTokenMap(t.tokens) &&
  isTokenMap(t.overlay);

const isProvider = (p) =>
  hasExactly(p, ['id', 'label', 'base_url', 'model', 'default_base_url', 'default_model', 'has_key']) &&
  isId(p.id) &&
  isString(p.label, 64) &&
  isString(p.base_url, MAX_BASE_URL) &&
  isString(p.model, 128) &&
  isString(p.default_base_url, MAX_BASE_URL) &&
  isString(p.default_model, 128) &&
  isBool(p.has_key);

const isHotkeyState = (h) =>
  hasExactly(h, ['requested', 'active', 'status', 'message']) &&
  isString(h.requested, 64) &&
  isString(h.active, 64) &&
  ['none', 'active', 'taken', 'invalid', 'failed'].includes(h.status) &&
  isString(h.message, 400);

const isPanelState = (s) =>
  hasExactly(s, ['hide_on_click_away', 'pinned']) && isBool(s.hide_on_click_away) && isBool(s.pinned);

const isHistoryMessage = (m) =>
  hasExactly(m, ['role', 'text']) && ['user', 'assistant'].includes(m.role) && isString(m.text);

const INCOMING = {
  'app.state': (p) =>
    hasExactly(p, ['app_version', 'themes', 'theme', 'provider', 'providers', 'busy', 'hotkey', 'panel', 'muted']) &&
    isHotkeyState(p.hotkey) &&
    isPanelState(p.panel) &&
    isBool(p.muted) &&
    isString(p.app_version, 32) &&
    Array.isArray(p.themes) &&
    p.themes.length > 0 &&
    p.themes.length <= 64 &&
    p.themes.every(isTheme) &&
    isId(p.theme) &&
    isId(p.provider) &&
    Array.isArray(p.providers) &&
    p.providers.length > 0 &&
    p.providers.length <= 32 &&
    p.providers.every(isProvider) &&
    isBool(p.busy),
  'chat.started': (p) =>
    hasExactly(p, ['request_id', 'provider', 'model', 'text']) &&
    isId(p.request_id) &&
    isId(p.provider) &&
    isString(p.model, 128) &&
    isString(p.text),
  'chat.history': (p) =>
    hasExactly(p, ['messages', 'active_request', 'active_model', 'partial']) &&
    Array.isArray(p.messages) &&
    p.messages.length <= 10000 &&
    p.messages.every(isHistoryMessage) &&
    (p.active_request === null || isId(p.active_request)) &&
    isString(p.active_model, 128) &&
    isString(p.partial),
  'ui.focus_input': (p) => hasExactly(p, []),
  'ui.show_settings': (p) => hasExactly(p, []),
  'chat.delta': (p) => hasExactly(p, ['request_id', 'text']) && isId(p.request_id) && isString(p.text),
  'chat.done': (p) =>
    hasExactly(p, ['request_id', 'stop_reason', 'input_tokens', 'output_tokens']) &&
    isId(p.request_id) &&
    isString(p.stop_reason, 64) &&
    isCountOrNull(p.input_tokens) &&
    isCountOrNull(p.output_tokens),
  'chat.error': (p) =>
    hasExactly(p, ['request_id', 'code', 'message']) && isId(p.request_id) && isString(p.code, 64) && isString(p.message, 4000),
  'chat.cleared': (p) => hasExactly(p, []),
  'host.error': (p) => hasExactly(p, ['code', 'message']) && isString(p.code, 64) && isString(p.message, 4000),
};

// Checks a message from the host. Returns { ok: true, type, payload } or { ok: false, error }.
export function validateHostMessage(message) {
  if (!hasExactly(message, ['v', 'type', 'payload'])) return { ok: false, error: 'invalid envelope' };
  if (message.v !== PROTOCOL_VERSION) return { ok: false, error: 'unsupported protocol version' };
  if (typeof message.type !== 'string') return { ok: false, error: 'invalid type' };
  const check = Object.prototype.hasOwnProperty.call(INCOMING, message.type) ? INCOMING[message.type] : null;
  if (!check) return { ok: false, error: `unknown message type: ${message.type.slice(0, 64)}` };
  if (!check(message.payload)) return { ok: false, error: `invalid payload for ${message.type}` };
  return { ok: true, type: message.type, payload: message.payload };
}
