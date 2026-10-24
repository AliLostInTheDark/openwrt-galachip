# SPDX-License-Identifier: Apache-2.0
"""Data-driven unit tests of onusys/model.js (LED modes and login accounts)."""
import sys
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

HERE = Path(__file__).resolve().parent
PKG = HERE.parent
sys.path.insert(0, str(PKG.parent / "luci-theme-slate" / "tests"))
import pyluci  # noqa: E402

MODEL = PKG / "htdocs/luci-static/resources/onusys/model.js"
PAGE = None
PASSWD = [
    "root:x:0:0:root:/root:/bin/ash",
    "daemon:*:1:1:daemon:/var:/bin/false",
    "ftp:*:55:55:ftp:/home/ftp:/bin/false",
    "network:*:101:101:network:/var:/bin/false",
    "nobody:*:65534:65534:nobody:/var:/bin/false",
    "ntp:x:123:123:ntp:/var/run/ntp:/bin/false",
    "logd:x:514:514:logd:/var:/bin/false",
    "ubus:x:81:81:ubus:/var/run/ubus:/bin/false",
    "dnsmasq:x:453:453:dnsmasq:/var/run/dnsmasq:/sbin/nologin",
]
TRIGGERS = ["none", "timer", "heartbeat", "default-on", "netdev"]


def setUpModule():
    global PAGE
    PAGE = pyluci.stub_page()
    PAGE.evaluate("src => { window.model = loadModule(src); }", pyluci.source(MODEL))


def tearDownModule():
    PAGE.context.close()
    pyluci.close()


def call(fn, *args):
    return PAGE.evaluate("([fn, args]) => model[fn](...args)", [fn, list(args)])


def raises(fn, *args):
    return PAGE.evaluate("([fn, args]) => { try { model[fn](...args); return null; } catch (e) { return e.message; } }", [fn, list(args)])


@ddt
class Names(unittest.TestCase):
    @data(
        ("green:pon", {"color": "green", "fn": "pon"}),
        ("red:los", {"color": "red", "fn": "los"}),
        ("tplink:green:lan", {"color": "green", "fn": "lan"}),
        ("led0", {"color": None, "fn": "led0"}),
        (":power", {"color": None, "fn": "power"}),
        ("a:b:c:d", {"color": "b", "fn": "c:d"}),
    )
    @unpack
    def test_parseName(self, name, want):
        self.assertEqual(call("parseName", name), want)

    @data(("green:power", "Power"), ("green:lan", "LAN"), ("green:pon", "PON"), ("red:los", "LOS"),
          ("blue:wlan", "Wi-Fi"), ("green:usb_port-2", "Usb port 2"), ("led0", "Led0"))
    @unpack
    def test_label(self, name, want):
        self.assertEqual(call("label", name), want)

    @data(("green:pon", True), ("red:los", True), ("green:fiber", True), ("green:lan", False), ("green:power", False))
    @unpack
    def test_isDriverLed(self, name, want):
        self.assertIs(call("isDriverLed", name), want)

    @data(
        (["green:lan", "red:los", "green:power", "green:pon"], ["green:power", "green:pon", "red:los", "green:lan"]),
        (["b:x", "a:x", "green:lan"], ["green:lan", "a:x", "b:x"]),
        ([], []),
    )
    @unpack
    def test_sortNames(self, names, want):
        self.assertEqual(call("sortNames", names), want)


@ddt
class Modes(unittest.TestCase):
    @data(
        ("green:pon", TRIGGERS, ["auto", "on", "off"]),
        ("red:los", [], ["auto", "on", "off"]),
        ("green:lan", TRIGGERS, ["default", "on", "off", "netdev", "heartbeat", "timer"]),
        ("green:power", ["none", "default-on"], ["default", "on", "off"]),
        ("green:power", None, ["default", "on", "off"]),
    )
    @unpack
    def test_modesFor(self, name, triggers, want):
        self.assertEqual(call("modesFor", name, triggers), want)

    @data(
        ("green:pon", None, "auto"),
        ("green:lan", None, "default"),
        ("green:pon", {"trigger": "none", "default": "1", "hold": "1"}, "on"),
        ("green:pon", {"trigger": "none", "default": "0", "hold": "1"}, "off"),
        ("green:pon", {"trigger": "timer"}, "custom"),
        ("green:lan", {"trigger": "netdev", "dev": "eth0"}, "netdev"),
        ("green:lan", {"trigger": "default-on"}, "on"),
        ("green:lan", {"trigger": "none", "default": "1"}, "on"),
        ("green:lan", {"trigger": "none"}, "off"),
        ("green:lan", {}, "off"),
        ("green:lan", {"trigger": "heartbeat"}, "heartbeat"),
        ("green:lan", {"trigger": "timer"}, "timer"),
        ("green:lan", {"trigger": "usbport"}, "custom"),
    )
    @unpack
    def test_modeOf(self, name, section, want):
        self.assertEqual(call("modeOf", name, section), want)

    @data(
        ("green:pon", "auto", None, None),
        ("green:lan", "default", None, None),
        ("green:pon", "on", None, {"name": "PON", "sysfs": "green:pon", "trigger": "none", "default": "1", "hold": "1"}),
        ("red:los", "off", None, {"name": "LOS", "sysfs": "red:los", "trigger": "none", "default": "0", "hold": "1"}),
        ("green:power", "on", None, {"name": "Power", "sysfs": "green:power", "trigger": "default-on", "default": "1"}),
        ("green:power", "off", None, {"name": "Power", "sysfs": "green:power", "trigger": "none", "default": "0"}),
        ("green:lan", "netdev", None, {"name": "LAN", "sysfs": "green:lan", "trigger": "netdev", "dev": "eth0", "mode": "link tx rx"}),
        ("green:lan", "netdev", {"dev": "br-lan"}, {"name": "LAN", "sysfs": "green:lan", "trigger": "netdev", "dev": "br-lan", "mode": "link tx rx"}),
        ("green:lan", "heartbeat", None, {"name": "LAN", "sysfs": "green:lan", "trigger": "heartbeat"}),
        ("green:lan", "timer", None, {"name": "LAN", "sysfs": "green:lan", "trigger": "timer", "delayon": "500", "delayoff": "500"}),
    )
    @unpack
    def test_sectionValues(self, name, mode, opts, want):
        self.assertEqual(call("sectionValues", name, mode, opts), want)

    @data(("green:pon", "netdev"), ("green:lan", "auto-ish"), ("green:pon", "heartbeat"))
    @unpack
    def test_sectionValues_rejects(self, name, mode):
        self.assertIn("unsupported mode", raises("sectionValues", name, mode, None) or "")

    @data(*[(n, m) for n in ("green:pon", "red:los", "green:lan", "green:power") for m in ("on", "off")])
    @unpack
    def test_round_trip(self, name, mode):
        # writing a mode and reading it back gives the same mode
        self.assertEqual(call("modeOf", name, call("sectionValues", name, mode, None)), mode)

    @data(("green:pon", TRIGGERS, "auto"), ("red:los", TRIGGERS, "auto"), ("green:lan", TRIGGERS, "netdev"),
          ("green:lan", ["none"], "default"), ("green:power", TRIGGERS, "on"), ("green:usb", TRIGGERS, "default"))
    @unpack
    def test_defaultMode(self, name, triggers, want):
        self.assertEqual(call("defaultMode", name, triggers), want)

    @data(
        ("green:pon", [], "led_pon"),
        ("green:pon", ["led_pon"], "led_pon_2"),
        ("green:pon", ["led_pon", "led_pon_2"], "led_pon_3"),
        ("tplink:green:usb-port.1", None, "led_usb_port_1"),
        ("green:LAN", [], "led_lan"),
    )
    @unpack
    def test_sectionName(self, name, existing, want):
        self.assertEqual(call("sectionName", name, existing), want)

    @data(
        ({"brightness": 1, "active_trigger": None}, "on"),
        ({"brightness": 0}, "off"),
        ({"brightness": 1, "active_trigger": "timer"}, "blinking"),
        ({"brightness": 0, "active_trigger": "heartbeat"}, "blinking"),
        ({"brightness": 1, "active_trigger": "netdev"}, "activity"),
        (None, "off"),
    )
    @unpack
    def test_stateOf(self, led, want):
        self.assertEqual(call("stateOf", led), want)


@ddt
class Accounts(unittest.TestCase):
    @data(
        (PASSWD, ["root"]),                                          # the stock image: only root
        (PASSWD + ["alice:x:1000:1000::/home/alice:/bin/ash"], ["root", "alice"]),
        (["alice:x:1000:1000::/home/alice:/bin/sh", "root:x:0:0:root:/root:/bin/ash"], ["root", "alice"]),
        (["bob:x:1:1::/:/usr/sbin/nologin", "carol:x:2:2::/:/sbin/nologin"], []),
        (["broken line", "", "root:x:0:0:root:/root:/bin/ash", "root:x:0:0:root:/root:/bin/ash"], ["root"]),
        (None, []),
    )
    @unpack
    def test_loginUsers(self, lines, want):
        self.assertEqual(call("loginUsers", lines), want)

    @data((["root", "admin", "admin", "", None], ["admin"]), (["root"], []), (None, []))
    @unpack
    def test_rpcdUsers(self, names, want):
        self.assertEqual(call("rpcdUsers", names), want)


if __name__ == "__main__":
    unittest.main()
