# SPDX-License-Identifier: Apache-2.0
"""Data-driven tests of slate-changes.js (Save & Apply in dialogs, staged-changes bar).

Run:  python -m unittest discover -s package/luci/luci-theme-slate/tests -p "test_*.py"
Needs: pip install ddt playwright (and Chromium, Chrome or Edge).
"""
import sys
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pyluci  # noqa: E402

MODULE = HERE.parent / "htdocs/luci-static/resources/slate-changes.js"
PAGE = None


def setUpModule():
    global PAGE
    PAGE = pyluci.stub_page()


def tearDownModule():
    PAGE.context.close()
    pyluci.close()


def fresh(initial_changes=None):
    """Reset the page, set ui.changes.changes and load the module into window.sc."""
    PAGE.evaluate("""([src, initial]) => {
        document.body.replaceChildren(); document.body.className = ''; calls.length = 0;
        ui.changes.changes = initial || {};
        window.sc = loadModule(src);
    }""", [pyluci.source(MODULE), initial_changes])


def js(expr, arg=None):
    return PAGE.evaluate(expr, arg)


BAR = """() => { const b = document.querySelector('.slate-applybar');
  return b ? {shown: !b.hidden, text: b.querySelector('.slate-applybar-text').textContent,
              padded: document.body.classList.contains('slate-has-applybar')} : null; }"""

# Builds a LuCI-style form dialog: Dismiss + Save; Save "succeeds" (hides the dialog
# after an async step, like handleModalSave) unless window.saveFails is set.
OPEN_DIALOG = """([cls, saveText, disabled]) => {
  window.saveFails = false;
  const dismiss = E('button', {class: 'btn cbi-button', click: () => ui.hideModal()}, ['Dismiss']);
  const save = E('button', {class: 'btn cbi-button cbi-button-positive important', disabled: disabled ? '' : null,
                            click: () => { calls.push(['save']); Promise.resolve().then(() => { if (!window.saveFails) ui.hideModal(); }); }},
                 [saveText]);
  return ui.showModal('Edit', [E('div', {}, ['fields']), E('div', {class: 'button-row'}, [dismiss, ' ', save])], ...cls).id;
}"""


def tick():
    js("() => new Promise(r => setTimeout(r, 20))")


@ddt
class Counting(unittest.TestCase):
    def setUp(self):
        fresh()

    @data(
        (None, 0), ({}, 0), ({"network": [["set", "lan", "x", "1"]]}, 1),
        ({"network": [[1], [2]], "system": [[3]]}, 3), ({"network": "bad"}, 0), ({"a": []}, 0),
    )
    @unpack
    def test_countChanges(self, changes, want):
        self.assertEqual(js("c => sc.countChanges(c)", changes), want)

    @data((1, "1 unsaved change"), (2, "2 unsaved changes"), (12, "12 unsaved changes"))
    @unpack
    def test_label(self, n, want):
        self.assertEqual(js("n => sc.label(n)", n), want)


@ddt
class Bar(unittest.TestCase):
    @data(
        (0, None),
        (1, {"shown": True, "text": "1 unsaved change", "padded": True}),
        (3, {"shown": True, "text": "3 unsaved changes", "padded": True}),
    )
    @unpack
    def test_setIndicator_drives_bar(self, n, want):
        fresh()
        js("n => ui.changes.setIndicator(n)", n)
        self.assertEqual(js(BAR), want)
        self.assertIn(["setIndicator", n], js("calls"), "LuCI's own indicator still runs")

    def test_bar_hides_when_changes_gone(self):
        fresh()
        js("ui.changes.setIndicator(2); ui.changes.setIndicator(0)")
        self.assertEqual(js(BAR), {"shown": False, "text": "0 unsaved changes", "padded": False})

    @data(({"system": [[1], [2]]}, "2 unsaved changes"), ({}, None))
    @unpack
    def test_initial_count_at_load(self, changes, text):
        fresh(changes)
        bar = js(BAR)
        self.assertEqual(bar and bar["text"], text)

    def test_hidden_while_page_button_visible(self):
        fresh()
        js("sc.pageApplyVisible = true; sc.count = 2; sc.render()")
        self.assertIsNone(js(BAR), "no bar is created while the page's own button is in view")
        js("sc.pageApplyVisible = false; sc.render()")
        self.assertTrue(js(BAR)["shown"])

    @data(("Review", ["displayChanges"]), ("Revert", ["revert"]), ("Save & Apply", ["apply", True]))
    @unpack
    def test_bar_buttons(self, label, call):
        fresh()
        js("ui.changes.setIndicator(1); calls.length = 0")
        js("l => [...document.querySelectorAll('.slate-applybar button')].find(b => b.textContent == l).click()", label)
        self.assertEqual(js("calls"), [call])


@ddt
class ApplyNow(unittest.TestCase):
    @data((True, False, ["page"]), (False, True, [["apply", True]]), (False, False, []))
    @unpack
    def test_applyNow(self, page_button, staged, want):
        fresh()
        js("""([pb, st]) => {
            if (pb) document.body.append(E('div', {class: 'cbi-page-actions'},
                     [E('button', {class: 'cbi-button cbi-button-apply', click: () => calls.push('page')}, ['Save & Apply'])]));
            sc.count = st ? 1 : 0; calls.length = 0; sc.applyNow(); }""", [page_button, staged])
        self.assertEqual(js("calls"), want)

    def test_disabled_page_button_falls_back(self):
        fresh()
        js("""() => { document.body.append(E('div', {class: 'cbi-page-actions'},
                 [E('button', {class: 'cbi-button cbi-button-apply', disabled: '', click: () => calls.push('page')}, ['x'])]));
                 sc.count = 1; calls.length = 0; sc.applyNow(); }""")
        self.assertEqual(js("calls"), [["apply", True]])


@ddt
class Dialogs(unittest.TestCase):
    def setUp(self):
        fresh()

    def buttons(self):
        return js("[...document.querySelectorAll('#modal .button-row button')].map(b => [b.textContent, b.className.includes('important'), b.disabled])")

    @data(
        (["cbi-modal"], "Save", False, [["Dismiss", False, False], ["Save", False, False], ["Save & Apply", True, False]]),
        (["cbi-modal"], "Save", True, [["Dismiss", False, False], ["Save", False, True], ["Save & Apply", True, True]]),
        ([], "Save", False, [["Dismiss", False, False], ["Save", True, False]]),            # not a form dialog
        (["cbi-modal"], "Create", False, [["Dismiss", False, False], ["Create", True, False]]),  # no Save button
    )
    @unpack
    def test_enhancement(self, classes, save_text, disabled, want):
        js(OPEN_DIALOG, [classes, save_text, disabled])
        self.assertEqual(self.buttons(), want)

    def test_enhance_is_idempotent(self):
        js(OPEN_DIALOG, [["cbi-modal"], "Save", False])
        js("sc.enhanceModal(document.querySelector('#modal'))")
        self.assertEqual(js("document.querySelectorAll('#modal .slate-save-apply').length"), 1)

    @data(
        # (click, save fails?, then click, want apply)
        ("Save & Apply", False, None, True),        # saved and closed: apply
        ("Save", False, None, False),               # plain save: stage only
        ("Save & Apply", True, None, False),        # validation failed: dialog stays, no apply
        ("Save & Apply", True, "Dismiss", False),   # then dismissed: still no apply
        ("Save & Apply", True, "Save", False),      # then plain save succeeds: no apply
        ("Dismiss", False, None, False),
    )
    @unpack
    def test_flows(self, first, fails, second, applied):
        js("() => { const b = E('button', {class: 'cbi-button cbi-button-apply', click: () => calls.push('page-apply')}, ['Save & Apply']);"
           " document.body.append(E('div', {class: 'cbi-page-actions'}, [b])); }")
        js(OPEN_DIALOG, [["cbi-modal"], "Save", False])
        js("f => { window.saveFails = f; }", fails)
        click = "t => [...document.querySelectorAll('#modal .button-row button')].find(b => b.textContent == t).click()"
        js(click, first)
        tick()
        if second:
            js("() => { window.saveFails = false; }")
            js(click, second)
            tick()
        self.assertEqual("page-apply" in js("calls"), applied, js("calls"))

    def test_save_and_apply_clicks_save(self):
        js(OPEN_DIALOG, [["cbi-modal"], "Save", False])
        js("document.querySelector('#modal .slate-save-apply').click()")
        self.assertIn(["save"], js("calls"))

    def test_disabled_does_nothing(self):
        js(OPEN_DIALOG, [["cbi-modal"], "Save", True])
        js("document.querySelector('#modal .slate-save-apply').click()")
        tick()
        self.assertNotIn(["save"], js("calls"))
        self.assertFalse(js("sc.pendingApply"))

    def test_new_dialog_clears_pending(self):
        js("sc.pendingApply = true")
        js(OPEN_DIALOG, [["cbi-modal"], "Save", False])
        self.assertFalse(js("sc.pendingApply"))


class Layout(unittest.TestCase):
    """The bar's own CSS: inside the viewport at phone and desktop widths."""

    def test_bar_fits(self):
        css = (HERE.parent / "htdocs/luci-static/slate/cascade.css").read_text(encoding="utf-8")
        for w in (320, 375, 600, 961, 1440):
            page = pyluci.stub_page(w, 700)
            try:
                page.add_style_tag(content=css)
                page.evaluate("src => { window.sc = loadModule(src); ui.changes.setIndicator(4); }", pyluci.source(MODULE))
                box = page.locator(".slate-applybar").bounding_box()
                self.assertGreaterEqual(box["x"], 0, w)
                self.assertLessEqual(box["x"] + box["width"], w, w)
                self.assertLessEqual(box["y"] + box["height"], 700, w)
                for b in page.locator(".slate-applybar button").all():
                    bb = b.bounding_box()
                    self.assertLessEqual(bb["x"] + bb["width"], w, (w, b.inner_text()))
            finally:
                page.context.close()


if __name__ == "__main__":
    unittest.main()
