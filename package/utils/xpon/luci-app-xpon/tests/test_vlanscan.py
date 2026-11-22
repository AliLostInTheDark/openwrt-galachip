# SPDX-License-Identifier: Apache-2.0
"""Data-driven tests of the VLAN scan table (view/omci/config.js) and the
route/priority wording it takes from omci/mib.js.

The real sources run in Chromium against luci-theme-slate's LuCI stand-in;
VLAN, GEM and Alloc-ID numbers are made up.

  pip install ddt playwright && python -m unittest discover -s tests -p "test_*.py"
"""
import sys
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

HERE = Path(__file__).resolve().parent
RES = HERE.parent / "htdocs/luci-static/resources"
sys.path.insert(0, str(HERE.parents[3] / "luci" / "luci-theme-slate" / "tests"))
import pyluci  # noqa: E402

PAGE = None

STUBS = r"""
window.view = { extend: proto => proto };
window.stub = {};
ui.createHandlerFn = (ctx, fn, ...a) => () => fn.apply(ctx, a);
window.dom = { content: (node, c) => node.replaceChildren(...[].concat(c)) };
"""


def keep(vid, pbit, set_to=None):
    """An OLT Extended VLAN rule: single-tagged frames on vid with pbit, priority set to set_to."""
    return {"filter_outer_pbit": 15, "filter_inner_pbit": pbit, "filter_inner_vid": vid,
            "treat_inner_pbit": pbit if set_to is None else set_to, "treat_inner_vid": vid, "tags_to_remove": 1}


def ext_vlan(*rules, origin="olt"):
    return {"class_id": 171, "entity_id": 1, "origin_name": origin, "extended_vlan": {"associated_me": 1, "rules": list(rules)}}


ROUTE = {"gems": {"1001": [0], "1002": [4], "1003": [5], "1004": [6], "1005": [7]}, "allocs": [1001, 1002, 1003, 1004, 1005]}
VLANS = {"321": {"sources": ["VLAN filter (ME 84)", "Extended VLAN filter (ME 171)", "Extended VLAN treatment (ME 171)"], "route": ROUTE}}
OBJECTS = [ext_vlan(*[keep(321, p) for p in (0, 4, 5, 6, 7)])]


def setUpModule():
    global PAGE
    PAGE = pyluci.stub_page()
    PAGE.add_script_tag(content=STUBS)
    src = pyluci.source(RES / "view/omci/config.js")
    hook = "return view.extend({"
    assert src.count(hook) == 1
    # expose the module's private table builder to the test
    src = src.replace(hook, "window.scan = { scanResults: scanResults };\n" + hook)
    PAGE.evaluate("""([mibSrc, cfgSrc]) => {
        window.mib = loadModule(mibSrc);
        loadModule(cfgSrc, {view, form: stub, uci: stub, fs: stub, dom, omci: stub, mib, widgets: stub});
    }""", [pyluci.source(RES / "omci/mib.js"), src])


def tearDownModule():
    PAGE.context.close()
    pyluci.close()


def call(expr, *args):
    return PAGE.evaluate(f"args => ({expr})(...args)", list(args))


def table(results, vlans, objects, scanned):
    """Render scanResults; rows as lists of cell lines (a <br> splits lines)."""
    return PAGE.evaluate("""([results, vlans, objects, scanned]) => {
        document.body.replaceChildren(scan.scanResults(results, 3, vlans, objects, scanned));
        const t = document.querySelector('table');
        if (!t) return { text: document.body.textContent };
        const cells = el => [...el.children].map(c => ({
            lines: c.innerHTML.split('<br>').map(s => s.replace(/<[^>]+>/g, '').trim()),
            nowrap: (c.getAttribute('style') || '').includes('nowrap'),
            title: c.dataset.title || null }));
        return { head: cells(t.rows[0]), rows: [...t.rows].slice(1).map(cells) };
    }""", [results, vlans, objects, scanned])


@ddt
class Wording(unittest.TestCase):
    @data(([], ""), ([5], "5"), ([0, 4, 5, 6, 7], "0, 4-7"), ([7, 0, 1, 2], "0-2, 7"),
          ([3, 3, 4], "3-4"), (["1006", "1007", "1008"], "1006-1008"), ([1, 3, 5], "1, 3, 5"))
    @unpack
    def test_ranges(self, numbers, text):
        self.assertEqual(call("n => mib.ranges(n)", numbers), text)

    @data((ROUTE, ["GEM 1001: priority 0", "GEM 1002: priority 4", "GEM 1003: priority 5", "GEM 1004: priority 6",
                   "GEM 1005: priority 7", "Alloc-ID 1001-1005"]),
          ({"gems": {"9": [0, 1, 2, 3, 4, 5, 6, 7]}, "allocs": [9]}, ["GEM 9: priority 0-7", "Alloc-ID 9"]),
          ({"gems": {"9": [1, 3]}, "allocs": []}, ["GEM 9: priority 1, 3"]),
          (None, ["no GEM port found"]))
    @unpack
    def test_route_lines(self, route, lines):
        self.assertEqual(call("r => mib.routeLines(r)", route), lines)
        self.assertEqual(call("r => mib.routeText(r)", route), ", ".join(lines))

    @data(
        # the screenshot's case: five one-priority rules that keep the priority become one line
        ([keep(321, p) for p in (0, 4, 5, 6, 7)], ["OLT: accepts priority 0, 4-7, keeps it"]),
        ([keep(321, 3)], ["OLT: accepts priority 3 only, keeps it"]),
        ([keep(321, 3), keep(321, 3)], ["OLT: accepts priority 3 only, keeps it"]),
        ([keep(321, 0, set_to=5)], ["OLT: accepts priority 0 only, sets it to 5"]),
        ([keep(321, 2), keep(321, 0, set_to=5)], ["OLT: accepts priority 2 only, keeps it", "OLT: accepts priority 0 only, sets it to 5"]),
        ([keep(321, 8, set_to=8)], ["OLT: accepts priority 0-7, keeps it"]),
        ([keep(4096, 8, set_to=3)], ["OLT: accepts priority 0-7, sets it to 3"]),
        ([keep(999, 3)], []),
        ([dict(keep(321, 3), delete=True)], []),
    )
    @unpack
    def test_olt_priority(self, rules, lines):
        self.assertEqual(call("(o, v) => mib.oltPriorityLines(o, v)", [ext_vlan(*rules)], 321), lines)
        self.assertEqual(call("(o, v) => mib.oltPriority(o, v)", [ext_vlan(*rules)], 321), "; ".join(lines))

    def test_rules_not_from_the_olt_ignored(self):
        self.assertEqual(call("(o, v) => mib.oltPriorityLines(o, v)", [ext_vlan(keep(321, 3), origin="default")], 321), [])


@ddt
class Table(unittest.TestCase):
    def test_provisioned_not_scanned(self):
        t = table([], VLANS, OBJECTS, False)
        self.assertEqual([h["lines"][0] for h in t["head"]], ["VLAN", "OLT rule", "GEM route", "Priority", "Service evidence"],
                         "no empty action column before a scan answers")
        row = t["rows"][0]
        self.assertEqual([c["lines"] for c in row], [
            ["321"],
            ["VLAN filter (ME 84)", "Extended VLAN filter (ME 171)", "Extended VLAN treatment (ME 171)"],
            ["GEM 1001: priority 0", "GEM 1002: priority 4", "GEM 1003: priority 5", "GEM 1004: priority 6",
             "GEM 1005: priority 7", "Alloc-ID 1001-1005"],
            ["OLT: accepts priority 0, 4-7, keeps it"],
            ["Provisioned; not probed yet"]])

    @data((0, True), (1, False), (2, True), (3, False), (4, False))
    @unpack
    def test_nowrap(self, col, nowrap):
        """The VLAN ID and the GEM lines never break; the prose columns wrap."""
        t = table([], VLANS, OBJECTS, False)
        self.assertEqual(t["rows"][0][col]["nowrap"], nowrap)
        self.assertEqual(t["head"][0]["nowrap"], True)

    def test_cells_titled_for_phones(self):
        t = table([], VLANS, OBJECTS, False)
        self.assertEqual([c["title"] for c in t["rows"][0]], ["VLAN", "OLT rule", "GEM route", "Priority", "Service evidence"])

    def test_answer_adds_action_column(self):
        answers = [{"vid": 321, "prio": 0, "reply_prio": 0, "type": "pppoe", "detail": "AC-Name bras1"}]
        t = table(answers, VLANS, OBJECTS, True)
        self.assertEqual(len(t["head"]), 6)
        row = t["rows"][0]
        self.assertEqual(row[3]["lines"], ["OLT: accepts priority 0, 4-7, keeps it", "BRAS probe PCP 0, reply PCP 0"])
        self.assertEqual(row[4]["lines"], ["Internet candidate: PPPoE BRAS: AC-Name bras1"])
        self.assertEqual(row[5]["lines"], ["Use tagged"])

    def test_scanned_silent_vlan_beside_answering_one(self):
        answers = [{"vid": 0, "prio": 0, "reply_prio": 0, "type": "dhcp", "detail": "offer 192.0.2.10"}]
        t = table(answers, VLANS, OBJECTS, True)
        self.assertEqual([r[0]["lines"][0] for r in t["rows"]], ["untagged", "321"])
        self.assertEqual(t["rows"][0][5]["lines"], ["Use transparent"])
        self.assertEqual(t["rows"][1][5]["lines"], [""])
        self.assertTrue(t["rows"][1][4]["lines"][0].startswith("No DHCPv4 or PPPoE reply."))

    def test_vlan_without_rule_uses_mapper(self):
        t = table([], VLANS, [], False)
        self.assertEqual(t["rows"][0][3]["lines"], ["OLT mapper: 0,4,5,6,7"])

    def test_nothing_provisioned(self):
        self.assertEqual(table([], {}, [], False), {"text": "The OLT has not provisioned any VLAN yet."})


if __name__ == "__main__":
    unittest.main()
