# SPDX-License-Identifier: Apache-2.0
"""Integration tests of the VLAN scan table on a real ONU running this firmware.

Skipped unless ONU_HOST is set, e.g.  ONU_HOST=192.0.2.1 python -m unittest tests/test_device.py
Needs SSH as root with key or empty-password login (as on a fresh image).

Read-only: it opens a LuCI session (destroyed at the end) and renders the
provisioned-VLAN table. It never starts a scan, which would pause switch
offload and the LAN VLAN setting.
"""
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[3] / "luci" / "luci-theme-slate" / "tests"))
import pyluci  # noqa: E402

HOST = os.environ.get("ONU_HOST")
SID = None


def ubus(path, method, args):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", f"root@{HOST}",
                        f"ubus call {path} {method} '{json.dumps(args)}'"], capture_output=True, text=True, timeout=30)
    return r.stdout.strip()


def setUpModule():
    global SID
    if not HOST:
        raise unittest.SkipTest("set ONU_HOST to run the device tests")
    sid = json.loads(ubus("session", "login", {"username": "root", "password": "", "timeout": 1800}) or "{}").get("ubus_rpc_session")
    if not sid:
        raise unittest.SkipTest("cannot open a LuCI session on the device (root password set?)")
    ubus("session", "set", {"ubus_rpc_session": sid, "values": {"token": os.urandom(16).hex()}})
    SID = sid


def tearDownModule():
    if SID:
        ubus("session", "destroy", {"ubus_rpc_session": SID})
    pyluci.close()


def scan_table(width, height):
    ctx = pyluci.browser().new_context(viewport={"width": width, "height": height},
                                       is_mobile=width < 768, has_touch=width < 768)
    ctx.add_cookies([{"name": "sysauth_http", "value": SID, "url": f"http://{HOST}"}])
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    try:
        for attempt in (1, 2):      # the ONU's uhttpd occasionally stalls a request
            try:
                page.goto(f"http://{HOST}/cgi-bin/luci/admin/network/omci/config", wait_until="domcontentloaded", timeout=45000)
                break
            except Exception:
                if attempt == 2:
                    raise
        page.wait_for_selector("#omci-vlanscan-results", state="attached", timeout=30000)
        page.get_by_text("VLAN scan", exact=True).first.click()     # the tab, so the table is laid out
        page.wait_for_function("() => { const r = document.querySelector('#omci-vlanscan-results');"
                               " return r && !/Reading the VLANs/.test(r.textContent); }", timeout=60000)
        info = page.evaluate("""() => {
            const r = document.querySelector('#omci-vlanscan-results'), t = r.querySelector('table');
            if (!t) return { text: r.textContent };
            /* text lines, from the text's own box: a cell stretches to its row's height */
            const lines = el => { const rg = document.createRange(); rg.selectNodeContents(el);
                return new Set([...rg.getClientRects()].map(r => Math.round(r.top))).size; };
            return {
                head: [...t.rows[0].cells].map(c => c.textContent.trim()),
                rows: [...t.rows].slice(1).map(tr => [...tr.cells].map(c => ({
                    lines: c.innerHTML.split('<br>').map(s => s.replace(/<[^>]+>/g, '').trim()),
                    tall: lines(c) }))),
                overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth
            };
        }""")
        info["errors"] = errors
        return info
    finally:
        ctx.close()


@ddt
class ScanTable(unittest.TestCase):
    @data(("desktop", 1280, 900), ("laptop", 1024, 768))
    @unpack
    def test_layout(self, name, width, height):
        t = scan_table(width, height)
        self.assertEqual(t["errors"], [])
        if "rows" not in t:
            self.skipTest("the OLT has not provisioned a VLAN: " + t["text"])
        self.assertEqual(t["head"], ["VLAN", "OLT rule", "GEM route", "Priority", "Service evidence"])
        for row in t["rows"]:
            vid = row[0]
            self.assertRegex(vid["lines"][0], r"^(\d{1,4}|untagged)$")
            self.assertLessEqual(vid["tall"], 1, f"{name}: VLAN {vid['lines'][0]} is broken over lines")
            for line in row[2]["lines"]:
                self.assertRegex(line, r"^(GEM \d+: priority [\d, -]+|Alloc-ID [\d, -]+|no GEM port found|-)$")
            prio = row[3]["lines"]
            self.assertEqual(len(prio), len(set(prio)))
            self.assertLessEqual(sum("keeps it" in p for p in prio), 1, "one-priority rules that keep it are merged")


if __name__ == "__main__":
    unittest.main()
