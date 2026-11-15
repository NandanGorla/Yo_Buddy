// Entry point for the Buddy UI: wires the page to the host bridge.
//
// The same page runs in two windows: the main window, and the floating panel (?view=panel),
// which shows the compact layout. Both receive every host message, so they stay in step.

import * as bridge from './bridge.js';
import { ChatView } from './chat.js';
import { applyTheme } from './theme.js';

const isPanel = new URLSearchParams(window.location.search).get('view') === 'panel';
document.documentElement.dataset.view = isPanel ? 'panel' : 'main';

const $ = (id) => document.getElementById(id);

const el = {
  modelBadge: $('model-badge'),
  themeSelect: $('theme-select'),
  newChat: $('new-chat-button'),
  settingsButton: $('settings-button'),
  settingsPanel: $('settings-panel'),
  pin: $('pin-button'),
  openMain: $('open-main-button'),
  hide: $('hide-button'),
  banner: $('banner'),
  composer: $('composer'),
  input: $('composer-input'),
  send: $('send-button'),
  stop: $('stop-button'),
  providerSelect: $('provider-select'),
  baseUrl: $('base-url-input'),
  model: $('model-input'),
  saveProvider: $('save-provider-button'),
  resetProvider: $('reset-provider-button'),
  keyStatus: $('key-status'),
  apiKey: $('api-key-input'),
  saveKey: $('save-key-button'),
  clearKey: $('clear-key-button'),
  hotkey: $('hotkey-input'),
  saveHotkey: $('save-hotkey-button'),
  hotkeyStatus: $('hotkey-status'),
  clickAway: $('click-away-checkbox'),
  pinned: $('pinned-checkbox'),
  versionLine: $('version-line'),
};

if (isPanel) el.input.placeholder = 'Ask Buddy';   // the long hint does not fit the compact layout

const chat = new ChatView($('messages'), $('empty-hint'));

let state = null;            // the latest "app.state" payload from the host
let busy = false;            // a reply is being streamed
let activeRequest = null;
let requestCounter = 0;
let bannerTimer = 0;
let lastHotkeyProblem = '';  // so the same hotkey warning is not repeated on every state update
const sentHere = new Set();  // request ids this window sent (their user text is already shown)

// ---- helpers ----

function showBanner(text, sticky = false) {
  el.banner.textContent = text;
  el.banner.hidden = false;
  clearTimeout(bannerTimer);
  if (!sticky) bannerTimer = setTimeout(() => { el.banner.hidden = true; }, 9000);
}

// Sends to the host and reports a protocol violation in the banner (it would be a UI bug).
function send(type, payload) {
  try {
    bridge.send(type, payload);
    return true;
  } catch (error) {
    showBanner(`This could not be sent: ${error.message}`);
    return false;
  }
}

function setBusy(value) {
  busy = value;
  el.send.disabled = value;
  el.stop.hidden = !value;
  el.newChat.disabled = value;
}

function nextRequestId() {
  requestCounter += 1;
  return `r_${isPanel ? 'p' : 'm'}_${Date.now().toString(36)}_${requestCounter}`;
}

function fillSelect(select, items, selectedId) {
  select.replaceChildren();
  for (const item of items) {
    const option = document.createElement('option');
    option.value = item.id;
    option.textContent = item.label;
    select.append(option);
  }
  select.value = selectedId;
}

function providerById(id) {
  return state ? state.providers.find((p) => p.id === id) : undefined;
}

// Shows one provider's stored settings in the settings form.
function showProviderInForm(provider) {
  el.baseUrl.value = provider.base_url;
  el.model.value = provider.model;
  el.keyStatus.textContent = provider.has_key
    ? `A key for ${provider.label} is stored.`
    : `No key stored for ${provider.label}.`;
  el.clearKey.disabled = !provider.has_key;
}

function autosizeInput() {
  el.input.style.height = 'auto';
  el.input.style.height = `${Math.min(el.input.scrollHeight, isPanel ? 120 : 200)}px`;
}

function focusInput() {
  el.input.focus();
}

function openSettings(open) {
  el.settingsPanel.hidden = !open;
  el.settingsButton.setAttribute('aria-expanded', String(open));
  if (open) el.providerSelect.focus();
}

function showHotkeyState(hotkey) {
  if (document.activeElement !== el.hotkey) el.hotkey.value = hotkey.active || hotkey.requested;
  if (hotkey.status === 'active') {
    el.hotkeyStatus.textContent = `${hotkey.active} shows and hides the panel.`;
    // A warning about the shortcut that is still on screen no longer applies.
    if (lastHotkeyProblem !== '' && el.banner.textContent === lastHotkeyProblem) el.banner.hidden = true;
    lastHotkeyProblem = '';
    return;
  }
  const working = hotkey.active ? '' : ' The panel has no working shortcut right now.';
  const text = (hotkey.message || 'The shortcut is not registered.') + working;
  el.hotkeyStatus.textContent = text;
  // Tell the user once, prominently, in the main window.
  if (!isPanel && text !== lastHotkeyProblem) {
    lastHotkeyProblem = text;
    showBanner(text, true);
  }
}

// ---- messages from the host ----

bridge.on('app.state', (payload) => {
  const firstState = state === null;
  const previousProvider = state ? el.providerSelect.value : null;
  state = payload;

  const theme = payload.themes.find((t) => t.id === payload.theme) || payload.themes[0];
  if (!applyTheme(theme)) {
    showBanner(`The theme "${theme.name}" has invalid values and was not applied.`);
    document.documentElement.dataset.theme = 'invalid';   // still reveal the page
  }
  fillSelect(el.themeSelect, payload.themes.map((t) => ({ id: t.id, label: t.name })), theme.id);

  const active = providerById(payload.provider) || payload.providers[0];
  el.modelBadge.textContent = `${active.label} · ${active.model || 'no model'}`;

  // Keep the provider the user is looking at in the form, if it still exists.
  const shown = providerById(previousProvider) || active;
  fillSelect(el.providerSelect, payload.providers.map((p) => ({ id: p.id, label: p.label })), shown.id);
  showProviderInForm(shown);

  showHotkeyState(payload.hotkey);
  el.clickAway.checked = payload.panel.hide_on_click_away;
  el.pinned.checked = payload.panel.pinned;
  el.pin.setAttribute('aria-pressed', String(payload.panel.pinned));
  el.pin.textContent = payload.panel.pinned ? 'Pinned' : 'Pin';

  el.versionLine.textContent = `Buddy ${payload.app_version}`;
  setBusy(payload.busy);

  // The page is hidden until its first theme arrives, and a hidden element cannot take focus.
  // So the first focus has to happen here, after the reveal.
  if (firstState) focusInput();
});

bridge.on('chat.history', (payload) => {
  const active = payload.active_request === null
    ? null
    : { requestId: payload.active_request, model: payload.active_model, partial: payload.partial };
  chat.showHistory(payload.messages, active);
  activeRequest = payload.active_request;
});

bridge.on('chat.started', (payload) => {
  // A question asked in the other window appears here too.
  if (!sentHere.has(payload.request_id)) chat.addUser(payload.text);
  sentHere.delete(payload.request_id);
  activeRequest = payload.request_id;
  setBusy(true);
  chat.startAssistant(payload.request_id, payload.model);
});

bridge.on('chat.delta', (payload) => {
  chat.appendDelta(payload.request_id, payload.text);
});

bridge.on('chat.done', (payload) => {
  const notes = [];
  if (payload.stop_reason === 'cancelled') notes.push('Stopped.');
  if (payload.stop_reason === 'length') notes.push('The reply reached the length limit and was cut off.');
  if (payload.input_tokens !== null && payload.output_tokens !== null) {
    notes.push(`${payload.input_tokens} tokens in, ${payload.output_tokens} out`);
  }
  chat.finish(payload.request_id, notes.join(' '));
  activeRequest = null;
  setBusy(false);
});

bridge.on('chat.error', (payload) => {
  // An error for a request this window did not send and never saw start is not shown here.
  const mine = sentHere.delete(payload.request_id) || payload.request_id === activeRequest;
  if (!mine) return;
  chat.fail(payload.request_id, payload.message || `The request failed (${payload.code}).`);
  if (payload.request_id === activeRequest || activeRequest === null) {
    activeRequest = null;
    setBusy(false);
  }
});

bridge.on('chat.cleared', () => {
  chat.clear();
});

bridge.on('host.error', (payload) => {
  if (!isPanel) showBanner(payload.message || payload.code);
});

bridge.on('ui.focus_input', focusInput);

bridge.on('ui.show_settings', () => {
  if (!isPanel) openSettings(true);
});

// ---- user actions ----

function submitMessage() {
  const text = el.input.value.trim();
  if (text.length === 0 || busy) return;
  const requestId = nextRequestId();
  sentHere.add(requestId);
  if (!send('chat.send', { request_id: requestId, text })) {
    sentHere.delete(requestId);
    return;
  }
  activeRequest = requestId;
  chat.addUser(text);
  el.input.value = '';
  autosizeInput();
  setBusy(true);
}

el.composer.addEventListener('submit', (event) => {
  event.preventDefault();
  submitMessage();
});

el.input.addEventListener('keydown', (event) => {
  // isComposing is true while an input method (for example for Japanese) is mid-word.
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    submitMessage();
  }
});

el.input.addEventListener('input', autosizeInput);

function cancelReply() {
  if (busy && activeRequest) send('chat.cancel', { request_id: activeRequest });
}

el.stop.addEventListener('click', cancelReply);

document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape' || event.isComposing) return;
  if (isPanel) {
    // In the panel Esc always means "get out of my way". A reply that is still streaming keeps
    // going and is there when the panel comes back; Stop is the way to cancel it.
    send('window.hide', {});
  } else {
    cancelReply();
  }
});

el.newChat.addEventListener('click', () => {
  send('chat.clear', {});
  focusInput();
});

el.themeSelect.addEventListener('change', () => {
  send('settings.set_theme', { theme: el.themeSelect.value });
});

el.settingsButton.addEventListener('click', () => {
  openSettings(el.settingsPanel.hidden);
});

el.providerSelect.addEventListener('change', () => {
  const provider = providerById(el.providerSelect.value);
  if (provider) showProviderInForm(provider);
});

el.saveProvider.addEventListener('click', () => {
  send('settings.set_provider', {
    provider: el.providerSelect.value,
    base_url: el.baseUrl.value.trim(),
    model: el.model.value.trim(),
  });
});

el.resetProvider.addEventListener('click', () => {
  const provider = providerById(el.providerSelect.value);
  if (!provider) return;
  el.baseUrl.value = provider.default_base_url;
  el.model.value = provider.default_model;
});

el.saveKey.addEventListener('click', () => {
  const key = el.apiKey.value.trim();
  if (key.length === 0) return;
  if (send('settings.set_api_key', { provider: el.providerSelect.value, key })) {
    el.apiKey.value = '';   // the key is not kept in the page once it has been handed over
  }
});

el.clearKey.addEventListener('click', () => {
  send('settings.clear_api_key', { provider: el.providerSelect.value });
});

// ---- floating panel settings and controls ----

function applyHotkey() {
  const text = el.hotkey.value.trim();
  if (text.length === 0) return;
  el.hotkey.blur();   // so the field shows what the host actually registered
  send('settings.set_hotkey', { hotkey: text });
}

el.saveHotkey.addEventListener('click', applyHotkey);
el.hotkey.addEventListener('keydown', (event) => {
  if (event.key === 'Enter') {
    event.preventDefault();
    applyHotkey();
  }
});

function sendPanelOptions(pinned) {
  send('settings.set_panel', { hide_on_click_away: el.clickAway.checked, pinned });
}

el.clickAway.addEventListener('change', () => sendPanelOptions(el.pinned.checked));
el.pinned.addEventListener('change', () => sendPanelOptions(el.pinned.checked));

el.pin.addEventListener('click', () => {
  sendPanelOptions(el.pin.getAttribute('aria-pressed') !== 'true');
  focusInput();
});

el.openMain.addEventListener('click', () => send('window.open_main', { settings: false }));
el.hide.addEventListener('click', () => send('window.hide', {}));

// ---- diagnostics: script errors and CSP violations go to the host's local log ----

window.addEventListener('error', (event) => {
  bridge.logToHost('error', `script error: ${event.message} (${event.filename}:${event.lineno})`);
});
window.addEventListener('unhandledrejection', (event) => {
  bridge.logToHost('error', `unhandled rejection: ${String(event.reason)}`);
});
document.addEventListener('securitypolicyviolation', (event) => {
  bridge.logToHost('warn', `CSP blocked ${event.violatedDirective}: ${event.blockedURI}`);
});

// ---- start ----

bridge.start();
if (bridge.hasHost) {
  send('app.ready', {});
} else {
  document.documentElement.dataset.theme = 'none';   // reveal the page so the notice is readable
  showBanner('This page is the Buddy user interface. It only works inside the Buddy app.', true);
}
