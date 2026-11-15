// The conversation view: user messages, streamed assistant replies and errors.

import { renderMarkdown } from './markdown.js';

const STICK_TO_BOTTOM_PX = 80;

export class ChatView {
  constructor(listElement, emptyHint) {
    this.list = listElement;
    this.emptyHint = emptyHint;
    this.active = null;   // the reply being streamed: { requestId, body, text, article, renderQueued }
  }

  addUser(text) {
    const { article, body } = this.#createMessage('user', 'You');
    body.textContent = text;   // user text is shown verbatim, never parsed
    this.#append(article);
  }

  startAssistant(requestId, modelName) {
    const { article, body } = this.#createMessage('assistant', `Buddy · ${modelName}`);
    body.classList.add('md');
    body.textContent = '…';
    this.#append(article);
    this.active = { requestId, article, body, text: '', renderQueued: false };
  }

  appendDelta(requestId, text) {
    const active = this.active;
    if (!active || active.requestId !== requestId) return;
    active.text += text;
    // Tokens can arrive many times per frame. Rendering once per frame keeps the UI smooth.
    if (!active.renderQueued) {
      active.renderQueued = true;
      requestAnimationFrame(() => {
        active.renderQueued = false;
        this.#render(active);
      });
    }
  }

  finish(requestId, note) {
    const active = this.active;
    if (!active || active.requestId !== requestId) return;
    this.#render(active);
    if (active.text.length === 0) active.body.textContent = '(no reply text)';
    if (note) this.#addNote(active.article, note);
    this.#scrollToBottom();
    this.active = null;
  }

  fail(requestId, message) {
    const active = this.active;
    if (active && active.requestId === requestId) {
      if (active.text.length > 0) {
        this.#render(active);
      } else {
        active.article.remove();
      }
      this.active = null;
    }
    const { article, body } = this.#createMessage('error', 'Problem');
    body.textContent = message;
    this.#append(article);
  }

  // Replaces the whole view with a stored conversation. `active`, when given, is the reply
  // that is still streaming: { requestId, model, partial }.
  showHistory(messages, active) {
    this.clear();
    for (const message of messages) {
      if (message.role === 'user') {
        this.addUser(message.text);
      } else {
        const { article, body } = this.#createMessage('assistant', 'Buddy');
        body.classList.add('md');
        renderMarkdown(body, message.text);
        this.#append(article);
      }
    }
    if (active) {
      this.startAssistant(active.requestId, active.model);
      if (active.partial.length > 0) this.appendDelta(active.requestId, active.partial);
    }
  }

  clear() {
    this.active = null;
    this.list.replaceChildren(this.emptyHint);
    this.emptyHint.hidden = false;
  }

  #render(active) {
    const stick = this.#isNearBottom();
    renderMarkdown(active.body, active.text);
    if (stick) this.#scrollToBottom();
  }

  #createMessage(kind, label) {
    const article = document.createElement('article');
    article.className = `msg msg-${kind}`;
    const meta = document.createElement('div');
    meta.className = 'msg-meta';
    meta.textContent = label;
    const body = document.createElement('div');
    body.className = 'msg-body';
    article.append(meta, body);
    return { article, body };
  }

  #addNote(article, text) {
    const note = document.createElement('div');
    note.className = 'msg-note';
    note.textContent = text;
    article.append(note);
  }

  #append(article) {
    this.emptyHint.hidden = true;
    this.list.append(article);
    this.#scrollToBottom();
  }

  #isNearBottom() {
    return this.list.scrollHeight - this.list.scrollTop - this.list.clientHeight < STICK_TO_BOTTOM_PX;
  }

  #scrollToBottom() {
    this.list.scrollTop = this.list.scrollHeight;
  }
}
