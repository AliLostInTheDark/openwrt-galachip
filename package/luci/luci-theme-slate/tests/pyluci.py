# SPDX-License-Identifier: Apache-2.0
"""Run LuCI JavaScript modules in a browser with a minimal LuCI stand-in.

Python tests (unittest + ddt) use this to load a module's real source and call it
without a router: baseclass, ui, L, E, _ and String.prototype.format are stubbed,
and every call to ui.changes / ui.showModal / ui.hideModal is recorded in
window.calls. Playwright drives its own Chromium when installed
(python -m playwright install chromium), otherwise an installed Chrome or Edge.
"""
import unittest
from pathlib import Path

STUB = r"""
window._ = s => s;
String.prototype.format = function(...a) { let i = 0; return this.replace(/%[sd]/g, () => String(a[i++])); };
window.E = (tag, attrs, children) => {
  /* like LuCI: E([], children) is a fragment, E(tag, children) has no attributes */
  if (Array.isArray(attrs) || typeof attrs == 'string' || attrs instanceof Node) { children = attrs; attrs = null; }
  if (Array.isArray(tag)) {
    const f = document.createDocumentFragment();
    for (const c of [].concat(children ?? [])) f.append(c instanceof Node ? c : document.createTextNode(String(c)));
    return f;
  }
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null) continue;
    if (typeof v == 'function') el.addEventListener(k, v); else el.setAttribute(k, v);
  }
  for (const c of [].concat(children ?? [])) el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return el;
};
window.baseclass = { extend(proto) { function C(...a) { if (this.__init__) this.__init__(...a); } Object.assign(C.prototype, proto); return C; } };
window.L = { env: { sessionid: 'test' } };
window.calls = [];
window.ui = {
  changes: {
    changes: {},
    setIndicator(n) { calls.push(['setIndicator', n]); },
    apply(checked) { calls.push(['apply', checked]); },
    revert() { calls.push(['revert']); },
    displayChanges() { calls.push(['displayChanges']); }
  },
  showModal(title, children, ...classes) {
    let d = document.querySelector('#modal');
    if (!d) { d = document.createElement('div'); d.id = 'modal'; document.body.append(d); }
    d.className = ['modal', ...classes].join(' ');
    d.replaceChildren(...children);
    document.body.classList.add('modal-overlay-active');
    calls.push(['showModal']);
    return d;
  },
  hideModal() { document.body.classList.remove('modal-overlay-active'); calls.push(['hideModal']); }
};
/* Evaluate a LuCI module source; a returned class is instantiated like L.require does. */
window.loadModule = (src, extra) => {
  const names = ['baseclass', 'ui', 'L', 'E', '_', ...Object.keys(extra || {})];
  const R = new Function(...names, src)(baseclass, ui, L, E, _, ...Object.values(extra || {}));
  return typeof R == 'function' ? new R() : R;
};
"""

_PW = _BROWSER = None


def browser():
    global _PW, _BROWSER
    if _BROWSER:
        return _BROWSER
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        raise unittest.SkipTest("playwright is not installed (pip install playwright)")
    _PW = sync_playwright().start()
    for kw in ({}, {"channel": "chrome"}, {"channel": "msedge"}):
        try:
            _BROWSER = _PW.chromium.launch(**kw)
            return _BROWSER
        except Exception:
            continue
    _PW.stop()
    _PW = None
    raise unittest.SkipTest("no Chromium, Chrome or Edge for Playwright")


def close():
    global _PW, _BROWSER
    if _BROWSER:
        _BROWSER.close()
    if _PW:
        _PW.stop()
    _PW = _BROWSER = None


def stub_page(width=1280, height=800):
    """A blank page with the LuCI stand-in installed."""
    ctx = browser().new_context(viewport={"width": width, "height": height})
    page = ctx.new_page()
    page.set_content("<!doctype html><html><head></head><body></body></html>")
    page.add_script_tag(content=STUB)
    return page


def source(path):
    return Path(path).read_text(encoding="utf-8")
