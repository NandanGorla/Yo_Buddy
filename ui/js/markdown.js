// Turns model output (Markdown) into DOM nodes that are safe to show.
//
// Model output is treated as hostile: a model can be tricked by text on a web page or in a file
// into emitting markup that tries to run script, load a tracking image or fake part of the UI.
// Three independent layers stand in the way:
//
//   1. marked converts Markdown to HTML. Raw HTML in the reply is escaped, so it is displayed
//      as text. Links and images are rendered as plain text that shows the real URL.
//   2. DOMPurify parses that HTML and keeps only a short allow-list of formatting tags and
//      attributes. Anything else is dropped.
//   3. The page's Content Security Policy blocks inline script, all network access, and (through
//      Trusted Types) any attempt to assign an unsanitised string to innerHTML.
//
// Nothing here uses innerHTML: DOMPurify hands back a DocumentFragment.

import { Marked } from '../vendor/marked-18.2.0/marked.esm.js';
import DOMPurify from '../vendor/dompurify-3.4.18/purify.es.js';

const HTML_ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const escapeHtml = (text) => String(text).replace(/[&<>"']/g, (c) => HTML_ESCAPES[c]);

const markdown = new Marked({ gfm: true, breaks: false, async: false });

markdown.use({
  renderer: {
    // Raw HTML written by the model is shown as text, never interpreted.
    html({ text, block }) {
      const escaped = escapeHtml(text);
      return block ? `<p>${escaped}</p>` : escaped;
    },
    // Links are not clickable. The label is followed by the real destination, so a link whose
    // label says one site and whose target is another cannot mislead.
    link({ href, text, tokens }) {
      const label = this.parser.parseInline(tokens);
      if (text === href) return `<span class="md-url">${escapeHtml(href)}</span>`;
      return `${label} <span class="md-url">(${escapeHtml(href)})</span>`;
    },
    // Images are never loaded: an image URL is a classic way to leak data.
    image({ href, text }) {
      return `<span class="md-url">[image: ${escapeHtml(text || '')} ${escapeHtml(href)}]</span>`;
    },
  },
});

const ALLOWED_TAGS = [
  'p', 'br', 'hr', 'strong', 'em', 'del', 'code', 'pre', 'blockquote',
  'ul', 'ol', 'li', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
  'table', 'thead', 'tbody', 'tr', 'th', 'td', 'span',
];
const ALLOWED_ATTR = ['class', 'align', 'start'];

const LANGUAGE_CLASS_RE = /^language-[A-Za-z0-9_+#.-]{1,32}$/;

// Attribute values are restricted too: "class" only in the two forms the renderer itself emits.
DOMPurify.addHook('uponSanitizeAttribute', (node, data) => {
  if (data.attrName === 'class') {
    const isLanguage = node.nodeName === 'CODE' && LANGUAGE_CLASS_RE.test(data.attrValue);
    const isUrl = node.nodeName === 'SPAN' && data.attrValue === 'md-url';
    if (!isLanguage && !isUrl) data.keepAttr = false;
  } else if (data.attrName === 'align') {
    if (!/^(?:left|center|right)$/.test(data.attrValue)) data.keepAttr = false;
  } else if (data.attrName === 'start') {
    if (!/^\d{1,9}$/.test(data.attrValue)) data.keepAttr = false;
  }
});

const PURIFY_CONFIG = {
  ALLOWED_TAGS,
  ALLOWED_ATTR,
  ALLOW_DATA_ATTR: false,
  ALLOW_ARIA_ATTR: false,
  RETURN_DOM_FRAGMENT: true,
};

// Renders Markdown text into `container`, replacing whatever was there.
export function renderMarkdown(container, text) {
  let fragment;
  try {
    const html = markdown.parse(text);
    fragment = DOMPurify.sanitize(html, PURIFY_CONFIG);
  } catch {
    // If parsing ever fails, fall back to showing the reply as plain text.
    fragment = document.createDocumentFragment();
    const plain = document.createElement('p');
    plain.textContent = text;
    fragment.append(plain);
  }
  container.replaceChildren(fragment);
}
