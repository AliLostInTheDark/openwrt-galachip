# SPDX-License-Identifier: Apache-2.0
"""Data-driven tests of view/onusys/leds.js: what it shows and what it writes to UCI.

The real view and model run in Chromium against a recording stand-in for LuCI's
uci, rpc and poll modules.
"""
import sys
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

HERE = Path(__file__).resolve().parent
PKG = HERE.parent
sys.path.insert(0, str(PKG.parent / "luci-theme-slate" / "tests"))
import pyluci  # noqa: E402

RES = PKG / "htdocs/luci-static/resources"
PAGE = None

G7_LEDS = {
    "green:lan": {"triggers": ["none", "timer", "heartbeat", "default-on", "netdev"], "active_trigger": "netdev", "brightness": 1, "max_brightness": 1},
    "green:pon": {"triggers": ["none", "timer", "heartbeat", "default-on", "netdev"], "brightness": 0, "max_brightness": 1},
    "green:power": {"triggers": ["none", "timer", "heartbeat", "default-on", "netdev"], "brightness": 1, "max_brightness": 1},
    "red:los": {"triggers": ["none", "timer", "heartbeat", "default-on", "netdev"], "brightness": 1, "max_brightness": 1},
}
G7_SECTIONS = [{".name": "led_lan", ".type": "led", "name": "LAN", "sysfs": "green:lan", "trigger": "netdev", "mode": "link tx rx", "dev": "eth0"}]

STUBS = r"""
window.ops = [];
window.uciData = [];
window.uci = {
  load: () => Promise.resolve(),
  sections: (conf, type) => uciData.filter(s => s['.type'] == type),
  add: (conf, type, name) => { uciData.push({'.name': name, '.type': type}); ops.push(['add', name]); return name; },
  set: (conf, sid, k, v) => { uciData.find(s => s['.name'] == sid)[k] = v; ops.push(['set', sid, k, v]); },
  unset: (conf, sid, k) => { const s = uciData.find(s => s['.name'] == sid); if (k in s) { delete s[k]; ops.push(['unset', sid, k]); } },
  remove: (conf, sid) => { uciData = uciData.filter(s => s['.name'] != sid); ops.push(['remove', sid]); },
  save: () => { ops.push(['save']); return Promise.resolve(); }
};
window.rpc = { declare: () => () => Promise.resolve(window.ledsData) };
window.poll = { add: () => {} };
window.view = { extend: proto => proto };
L.hasViewPermission = () => !window.readonlyUser;
"""


def setUpModule():
    global PAGE
    PAGE = pyluci.stub_page()
    PAGE.add_script_tag(content=STUBS)
    PAGE.evaluate("src => { window.model = loadModule(src); }", pyluci.source(RES / "onusys/model.js"))


def tearDownModule():
    PAGE.context.close()
    pyluci.close()


def render(leds, sections, readonly=False):
    PAGE.evaluate("""([src, leds, sections, ro]) => {
        document.body.replaceChildren(); ops.length = 0;
        window.ledsData = leds; window.uciData = JSON.parse(JSON.stringify(sections)); window.readonlyUser = ro;
        window.v = loadModule(src, {view, rpc, uci, poll, model});
        document.body.append(v.render([leds, null]));
    }""", [pyluci.source(RES / "view/onusys/leds.js"), leds, sections, readonly])


def rows():
    return PAGE.evaluate("""[...document.querySelectorAll('.onusys-led')].map(r => ({
        led: r.dataset.led, label: r.querySelector('label').textContent.trim(),
        mode: r.querySelector('select').value, modes: [...r.querySelector('select').options].map(o => o.value),
        state: r.querySelector('.onusys-state').textContent, disabled: r.querySelector('select').disabled}))""")


def choose(selections):
    PAGE.evaluate("sel => { for (const [led, m] of Object.entries(sel)) document.querySelector(`select[data-led='${led}']`).value = m; }", selections)


def save():
    PAGE.evaluate("() => v.handleSave()")
    return PAGE.evaluate("ops"), PAGE.evaluate("uciData")


@ddt
class Render(unittest.TestCase):
    def test_g7_rows(self):
        render(G7_LEDS, G7_SECTIONS)
        self.assertEqual([(r["led"], r["label"], r["mode"]) for r in rows()], [
            ("green:power", "Power", "default"), ("green:pon", "PON", "auto"),
            ("red:los", "LOS", "auto"), ("green:lan", "LAN", "netdev")])

    @data(("green:pon", ["auto", "on", "off"]), ("green:lan", ["default", "on", "off", "netdev", "heartbeat", "timer"]))
    @unpack
    def test_modes_offered(self, led, modes):
        render(G7_LEDS, G7_SECTIONS)
        self.assertEqual(next(r for r in rows() if r["led"] == led)["modes"], modes)

    @data(("green:lan", "Now: network activity"), ("green:pon", "Now: off"), ("red:los", "Now: on"))
    @unpack
    def test_live_state(self, led, text):
        render(G7_LEDS, G7_SECTIONS)
        self.assertEqual(next(r for r in rows() if r["led"] == led)["state"], text)

    def test_poll_updates_state(self):
        render(G7_LEDS, G7_SECTIONS)
        PAGE.evaluate("v.updateStates({'green:pon': {brightness: 1}})")
        self.assertEqual(next(r for r in rows() if r["led"] == "green:pon")["state"], "Now: on")

    def test_custom_trigger_kept(self):
        render(G7_LEDS, [{".name": "x", ".type": "led", "sysfs": "green:lan", "trigger": "usbport"}])
        lan = next(r for r in rows() if r["led"] == "green:lan")
        self.assertEqual(lan["mode"], "custom")
        self.assertIn("custom", lan["modes"])

    def test_no_leds(self):
        render({}, [])
        self.assertEqual(rows(), [])
        self.assertIn("no controllable LEDs", PAGE.inner_text("body"))

    def test_readonly(self):
        render(G7_LEDS, G7_SECTIONS, readonly=True)
        self.assertTrue(all(r["disabled"] for r in rows()))


@ddt
class Save(unittest.TestCase):
    @data(
        # nothing changed: nothing written but the save
        ({}, [["save"]], None),
        # PON always off: new held section
        ({"green:pon": "off"}, None, {"led_pon": {"name": "PON", "sysfs": "green:pon", "trigger": "none", "default": "0", "hold": "1"}}),
        # LOS always on
        ({"red:los": "on"}, None, {"led_los": {"name": "LOS", "sysfs": "red:los", "trigger": "none", "default": "1", "hold": "1"}}),
        # LAN off: existing section reused, netdev options removed
        ({"green:lan": "off"}, None, {"led_lan": {"name": "LAN", "sysfs": "green:lan", "trigger": "none", "default": "0"}}),
        # LAN heartbeat
        ({"green:lan": "heartbeat"}, None, {"led_lan": {"name": "LAN", "sysfs": "green:lan", "trigger": "heartbeat"}}),
        # power always on
        ({"green:power": "on"}, None, {"led_power": {"name": "Power", "sysfs": "green:power", "trigger": "default-on", "default": "1"}}),
        # LAN back to firmware default: section removed
        ({"green:lan": "default"}, [["remove", "led_lan"], ["save"]], {}),
    )
    @unpack
    def test_writes(self, selections, want_ops, want_sections):
        render(G7_LEDS, G7_SECTIONS)
        choose(selections)
        ops, data_ = save()
        if want_ops is not None:
            self.assertEqual(ops, want_ops)
        if want_sections is not None:
            got = {s[".name"]: {k: v for k, v in s.items() if not k.startswith(".")} for s in data_}
            for name, values in want_sections.items():
                self.assertEqual(got.get(name), values)
            if not want_sections:
                self.assertNotIn("led_lan", got)

    def test_held_back_to_auto_removes(self):
        render(G7_LEDS, G7_SECTIONS + [{".name": "led_pon", ".type": "led", "sysfs": "green:pon", "trigger": "none", "default": "0", "hold": "1"}])
        self.assertEqual(next(r for r in rows() if r["led"] == "green:pon")["mode"], "off")
        choose({"green:pon": "auto"})
        ops, data_ = save()
        self.assertEqual(ops, [["remove", "led_pon"], ["save"]])

    @data(
        ("All LEDs off", {"green:power": "off", "green:pon": "off", "red:los": "off", "green:lan": "off"}),
        ("Restore defaults", {"green:power": "on", "green:pon": "auto", "red:los": "auto", "green:lan": "netdev"}),
    )
    @unpack
    def test_bulk_buttons(self, button, want):
        render(G7_LEDS, G7_SECTIONS)
        choose({"green:lan": "timer", "green:pon": "on"})
        PAGE.evaluate("t => [...document.querySelectorAll('button')].find(b => b.textContent == t).click()", button)
        self.assertEqual({r["led"]: r["mode"] for r in rows()}, want)

    def test_all_off_then_save_writes_four_sections(self):
        render(G7_LEDS, G7_SECTIONS)
        PAGE.evaluate("[...document.querySelectorAll('button')].find(b => b.textContent == 'All LEDs off').click()")
        ops, data_ = save()
        self.assertEqual(sorted(s["sysfs"] for s in data_ if s.get("default") == "0"),
                         ["green:lan", "green:pon", "green:power", "red:los"])

    def test_new_section_names_do_not_clash(self):
        render(G7_LEDS, G7_SECTIONS + [{".name": "led_pon", ".type": "led", "sysfs": "other:thing"}])
        choose({"green:pon": "off"})
        ops, _ = save()
        self.assertIn(["add", "led_pon_2"], ops)


if __name__ == "__main__":
    unittest.main()
