# SPDX-License-Identifier: Apache-2.0
"""Integration tests on a real ONU running this firmware (and luci-theme-slate).

Skipped unless ONU_HOST is set, e.g.  ONU_HOST=192.0.2.1 python -m unittest ...
Needs SSH as root with key or empty-password login (as on a fresh image).

What it does on the device:
- opens a LuCI session through rpcd (ubus session login) and destroys it at the end
- reads pages; stages one network change and reverts it (never applied)
- applies LED settings (PON/LOS held) and restores automatic mode afterwards
It never changes passwords.
"""
import json
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "luci-theme-slate" / "tests"))
import pyluci  # noqa: E402

HOST = os.environ.get("ONU_HOST")
BASE = f"http://{HOST}/cgi-bin/luci/" if HOST else ""
SID = None
SIZES = {"desktop": (1280, 900), "phone": (390, 844)}


def ssh(cmd, timeout=30):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{HOST}", cmd],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def ubus(path, method, args):
    return ssh(f"ubus call {path} {method} '{json.dumps(args)}'")


def setUpModule():
    global SID
    if not HOST:
        raise unittest.SkipTest("set ONU_HOST to run the device tests")
    sid = json.loads(ubus("session", "login", {"username": "root", "password": "", "timeout": 1800}) or "{}").get("ubus_rpc_session")
    if not sid:
        raise unittest.SkipTest("cannot open a LuCI session on the device (root password set?)")
    ubus("session", "set", {"ubus_rpc_session": sid, "values": {"token": os.urandom(16).hex()}})
    SID = sid
    pyluci.browser()


def tearDownModule():
    if SID:
        ubus("uci", "revert", {"config": "network", "ubus_rpc_session": SID})
        ubus("session", "destroy", {"ubus_rpc_session": SID})
    pyluci.close()


def open_page(path, size=(1280, 900)):
    ctx = pyluci.browser().new_context(viewport={"width": size[0], "height": size[1]},
                                       is_mobile=size[0] < 768, has_touch=size[0] < 768)
    ctx.add_cookies([{"name": "sysauth_http", "value": SID, "url": f"http://{HOST}"}])
    page = ctx.new_page()
    page.errors = []
    page.on("pageerror", lambda e: page.errors.append(str(e)))
    # the ONU's single-process uhttpd occasionally stalls a request: retry once
    for attempt in (1, 2):
        try:
            page.goto(BASE + path, wait_until="domcontentloaded", timeout=45000)
            break
        except Exception:
            if attempt == 2:
                raise
    page.wait_for_function("document.querySelector('#view') && !document.querySelector('#view .spinning')", timeout=20000)
    page.wait_for_timeout(800)
    return page


def staged(config="network"):
    return json.loads(ubus("uci", "changes", {"config": config, "ubus_rpc_session": SID}) or "{}").get("changes", [])


@ddt
class Pages(unittest.TestCase):
    @data(*[(p, n, s) for p in ("admin/system/leds", "admin/system/admin/password", "admin/network/network", "admin/status/overview")
            for n, s in SIZES.items()])
    @unpack
    def test_loads_without_errors(self, path, name, size):
        page = open_page(path, size)
        try:
            self.assertEqual(page.errors, [], f"{path} @ {name}")
            self.assertNotIn("/login", page.url)
            over = page.evaluate("document.documentElement.scrollWidth - document.documentElement.clientWidth")
            self.assertLessEqual(over, 0, f"{path} @ {name} scrolls sideways")
        finally:
            page.context.close()

    def test_leds_page_lists_device_leds(self):
        leds = sorted(ssh("ls /sys/class/leds").split())
        page = open_page("admin/system/leds")
        try:
            shown = sorted(page.evaluate("[...document.querySelectorAll('.onusys-led')].map(r => r.dataset.led)"))
            self.assertEqual(shown, leds)
        finally:
            page.context.close()

    def test_password_page_only_login_accounts(self):
        logins = [l.split(":")[0] for l in ssh("cat /etc/passwd").splitlines()
                  if len(l.split(":")) >= 7 and not l.split(":")[6].endswith(("/false", "nologin"))]
        page = open_page("admin/system/admin/password")
        try:
            text = page.inner_text("#view")
            self.assertIn("New password", text)
            self.assertIn("Confirmation", text)
            for system_user in ("daemon", "network", "ubus", "logd", "nobody"):
                self.assertNotIn(system_user, page.evaluate("[...document.querySelectorAll('#view select option')].map(o => o.value)"))
            if logins == ["root"]:
                self.assertEqual(page.locator("[data-name='user']").count(), 0, "single account: no username field")
        finally:
            page.context.close()

    def test_menu_has_leds_entry(self):
        page = open_page("admin/status/overview")
        try:
            self.assertGreater(page.locator("a[href$='/admin/system/leds']").count(), 0)
        finally:
            page.context.close()


@ddt
class SaveAndApply(unittest.TestCase):
    def tearDown(self):
        ubus("uci", "revert", {"config": "network", "ubus_rpc_session": SID})

    @data(*SIZES.items())
    @unpack
    def test_dialog_button_applies(self, name, size):
        page = open_page("admin/network/network", size)
        try:
            # record the apply instead of applying it
            page.evaluate("L.require('ui').then(ui => { ui.changes.apply = c => { window.__applied = c; }; })")
            page.locator("#view button:has-text('Edit')").first.click()
            page.wait_for_selector(".modal .slate-save-apply", timeout=8000)
            labels = page.evaluate("[...document.querySelectorAll('.modal .button-row button')].map(b => b.textContent.trim())")
            self.assertEqual(labels[-2:], ["Save", "Save & Apply"], name)
            page.locator(".modal input[id$='.gateway']").first.fill("192.0.2.254")
            page.locator(".modal .slate-save-apply").click()
            page.wait_for_function("window.__applied !== undefined", timeout=10000)
            self.assertTrue(page.evaluate("window.__applied"), "checked apply, as the page's own button does")
            self.assertTrue(any(c[2] == "gateway" for c in staged()), "the dialog's change was staged first")
        finally:
            page.context.close()

    def test_bar_on_other_page_and_revert(self):
        ubus("uci", "set", {"config": "network", "section": "lan", "values": {"gateway": "192.0.2.254"}, "ubus_rpc_session": SID})
        page = open_page("admin/status/overview")
        try:
            page.wait_for_selector(".slate-applybar:not([hidden])", timeout=8000)
            self.assertIn("1 unsaved change", page.inner_text(".slate-applybar"))
            page.locator(".slate-applybar button:has-text('Revert')").click()
            for _ in range(80):        # tolerate the ONU's occasional request stall
                if not staged():
                    break
                time.sleep(0.5)
            self.assertEqual(staged(), [])
        finally:
            page.context.close()


class LedApply(unittest.TestCase):
    """Real Save & Apply of PON/LOS held states, then back to automatic."""

    def running(self):
        """True while procd has a running led-hold instance (pgrep -f would match this ssh command)."""
        out = ubus("service", "list", {"name": "led-hold"})
        instances = (json.loads(out or "{}").get("led-hold") or {}).get("instances") or {}
        return any(i.get("running") and "--resync-later" not in i.get("command", []) for i in instances.values())

    def sample(self, led, n=6, gap=0.5):
        out = []
        for _ in range(n):
            out.append(ssh(f"cat /sys/class/leds/{led}/brightness"))
            time.sleep(gap)
        return out

    def settled(self, want, timeout=20):
        """Wait until every LED in want shows its value (the led restart and led-hold settle)."""
        end = time.time() + timeout
        while time.time() < end:
            if all(ssh(f"cat /sys/class/leds/{l}/brightness") == v for l, v in want.items()):
                return True
            time.sleep(1)
        return False

    def apply_modes(self, modes):
        page = open_page("admin/system/leds")
        try:
            page.evaluate("m => { for (const [led, v] of Object.entries(m)) document.querySelector(`select[data-led='${led}']`).value = v; }", modes)
            page.locator(".cbi-page-actions .cbi-button-apply").first.click()
            # a checked apply is rolled back unless the page confirms it: wait for LuCI's confirmation
            page.wait_for_function("document.body.innerText.includes('Configuration changes applied')", timeout=90000)
        finally:
            page.context.close()

    def test_hold_and_restore(self):
        leds = ssh("ls /sys/class/leds").split()
        if "green:pon" not in leds or "red:los" not in leds:
            self.skipTest("device has no PON/LOS LEDs")
        try:
            self.apply_modes({"green:pon": "on", "red:los": "off"})
            self.assertTrue(self.running(), "led-hold started by the apply")
            self.assertTrue(self.settled({"green:pon": "1", "red:los": "0"}))
            self.assertEqual(set(self.sample("green:pon")), {"1"}, "PON held on")
            self.assertEqual(set(self.sample("red:los")), {"0"}, "LOS held off (the driver blinks it without fibre)")
            self.assertEqual(ssh("uci -q get system.led_pon.hold; uci -q get system.led_los.hold").split(), ["1", "1"])
        finally:
            self.apply_modes({"green:pon": "auto", "red:los": "auto"})
        self.assertEqual(ssh("uci -q show system | grep -c \"hold='1'\""), "0")
        for _ in range(15):          # procd reloads the service shortly after the apply
            if not self.running():
                break
            time.sleep(1)
        self.assertFalse(self.running(), "led-hold stops when nothing is held")

        # back on automatic, the LEDs show the line state again (resync), as the driver would
        los, sig, state = ssh("cat /sys/class/xpon/pon0/los /sys/class/xpon/pon0/signal_detect /sys/class/xpon/pon0/state").split()
        if los == "1" or sig == "0":
            self.assertEqual(set(self.sample("green:pon", 4, 0.3)), {"0"}, "no light: PON off")
            self.assertEqual(len(set(self.sample("red:los", 10, 0.3))), 2, "no light: LOS blinking")
        elif state == "operational":
            self.assertEqual(set(self.sample("green:pon", 4, 0.3)), {"1"}, "registered: PON on")
            self.assertEqual(set(self.sample("red:los", 4, 0.3)), {"0"}, "light: LOS off")


if __name__ == "__main__":
    unittest.main()
