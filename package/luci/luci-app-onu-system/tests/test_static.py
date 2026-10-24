# SPDX-License-Identifier: Apache-2.0
"""Data-driven checks of the package files: menu, views, scripts, Makefile."""
import json
import re
import subprocess
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

PKG = Path(__file__).resolve().parent.parent
REPO = PKG.parents[2]
MENU = json.loads((PKG / "root/usr/share/luci/menu.d/zz-luci-app-onu-system.json").read_text(encoding="utf-8"))
VIEWS = PKG / "htdocs/luci-static/resources/view"


@ddt
class Menu(unittest.TestCase):
    @data(*MENU.items())
    @unpack
    def test_view_exists(self, path, node):
        self.assertEqual(node["action"]["type"], "view")
        self.assertTrue((VIEWS / (node["action"]["path"] + ".js")).is_file(), path)

    def test_sorts_after_luci_mod_system(self):
        # LuCI merges menu.d files in name order: a later file overrides a node's action
        self.assertGreater("zz-luci-app-onu-system.json", "luci-mod-system.json")

    def test_password_override_keeps_upstream_acl_and_title(self):
        node = MENU["admin/system/admin/password"]
        self.assertEqual(set(node), {"action"}, "only the action is replaced")

    def test_leds_page_needs_system_acl(self):
        self.assertEqual(MENU["admin/system/leds"]["depends"]["acl"], ["luci-mod-system-config"])


@ddt
class Sources(unittest.TestCase):
    @data(*sorted(p.relative_to(PKG).as_posix() for p in (PKG / "htdocs").rglob("*.js")))
    def test_luci_module_header(self, rel):
        src = (PKG / rel).read_text(encoding="utf-8")
        self.assertTrue(src.startswith("'use strict';\n'require "), rel)
        for req in re.findall(r"^'require ([\w.]+)", src, re.M):
            if req.startswith("onusys."):
                self.assertTrue((PKG / "htdocs/luci-static/resources" / (req.replace(".", "/") + ".js")).is_file(), req)

    @data("root/usr/libexec/led-hold", "root/etc/init.d/led-hold", "root/etc/uci-defaults/40_luci-app-onu-system")
    def test_executable_in_git(self, rel):
        out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-s", "--", f"package/luci/luci-app-onu-system/{rel}"],
                             capture_output=True, text=True).stdout
        if not out:
            self.skipTest("not committed yet")
        self.assertTrue(out.startswith("100755"), out)

    @data(("PKG_LICENSE", "Apache-2.0"), ("LUCI_PKGARCH", "all"))
    @unpack
    def test_makefile(self, key, value):
        self.assertRegex((PKG / "Makefile").read_text(encoding="utf-8"), rf"(?m)^{key}:={re.escape(value)}$")

    def test_makefile_includes_luci_mk(self):
        self.assertIn("include $(TOPDIR)/feeds/luci/luci.mk", (PKG / "Makefile").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
