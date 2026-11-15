// Applies a theme by setting CSS variables (--bd-<token>) on the root element.
//
// The host has already validated every theme file (src/core/theme.cpp). The same value rules are
// checked again here before anything is handed to CSS, so the UI does not depend on that.

const COLOR_RE = /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;
const LENGTH_RE = /^(?:0|\d{1,3}(?:\.\d{1,2})?(?:px|rem|em))$/;
const FONT_WORD_RE = /^[A-Za-z][A-Za-z0-9-]*$/;

function isFontList(value) {
  if (value.length === 0 || value.length > 200) return false;
  return value.split(',').every((name) => {
    const words = name.trim().split(' ').filter((w) => w.length > 0);
    return words.length > 0 && words.every((w) => FONT_WORD_RE.test(w));
  });
}

const isColor = (v) => COLOR_RE.test(v);
const isLength = (v) => LENGTH_RE.test(v);

// The token vocabulary. Mirrors kUiTokens in src/core/theme.cpp.
const TOKENS = {
  'bg': isColor,
  'surface': isColor,
  'surface-raised': isColor,
  'text': isColor,
  'text-muted': isColor,
  'accent': isColor,
  'accent-text': isColor,
  'border': isColor,
  'danger': isColor,
  'code-bg': isColor,
  'code-text': isColor,
  'radius': isLength,
  'font-ui': isFontList,
  'font-mono': isFontList,
};

// Returns true when the theme was applied, false when it was rejected (the old theme stays).
export function applyTheme(theme) {
  const names = Object.keys(TOKENS);
  const valid = names.every((name) => typeof theme.tokens[name] === 'string' && TOKENS[name](theme.tokens[name]));
  if (!valid) return false;

  const root = document.documentElement;
  for (const name of names) {
    root.style.setProperty(`--bd-${name}`, theme.tokens[name]);
  }
  root.dataset.kind = theme.kind;
  root.dataset.theme = theme.id;   // also reveals the page: see app.css
  return true;
}
