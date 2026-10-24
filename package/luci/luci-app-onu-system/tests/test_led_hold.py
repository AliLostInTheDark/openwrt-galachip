# SPDX-License-Identifier: Apache-2.0
"""Data-driven tests of /usr/libexec/led-hold against a fake /sys/class/leds tree.

A stand-in "uci" prints a prepared `uci show system`; LEDS_DIR and XPON_DIR point at
temporary directories. Needs a POSIX sh with awk (on Windows: Git for Windows). LED names use "_" instead
of ":" so the fake tree also works on Windows; the script treats names as opaque.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from ddt import data, ddt, unpack

PKG = Path(__file__).resolve().parent.parent
SCRIPT = PKG / "root/usr/libexec/led-hold"
SH = shutil.which("sh") or shutil.which("bash")

HOLD_PON_OFF = """system.led_pon=led
system.led_pon.name='PON'
system.led_pon.sysfs='green_pon'
system.led_pon.trigger='none'
system.led_pon.default='0'
system.led_pon.hold='1'
"""
HOLD_LOS_ON = """system.led_los=led
system.led_los.sysfs='red_los'
system.led_los.trigger='none'
system.led_los.default='1'
system.led_los.hold='1'
"""
LAN_NETDEV = """system.led_lan=led
system.led_lan.name='LAN'
system.led_lan.sysfs='green_lan'
system.led_lan.trigger='netdev'
system.led_lan.mode='link tx rx'
system.led_lan.dev='eth0'
"""
SYSTEM_BASE = "system.@system[0]=system\nsystem.@system[0].hostname='OpenWrt'\nsystem.ntp=timeserver\n"


@unittest.skipUnless(SH, "no POSIX shell")
@ddt
class LedHold(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.leds = self.tmp / "leds"
        self.xpon = self.tmp / "xpon"
        self.bin = self.tmp / "bin"
        for d in (self.leds, self.xpon, self.bin):
            d.mkdir()
        (self.xpon / "state").write_text("operational\n")
        (self.xpon / "los").write_text("0\n")
        uci = self.bin / "uci"
        uci.write_text('#!/bin/sh\ncat "$UCI_SHOW"\n', newline="\n")
        uci.chmod(0o755)
        self.show = self.tmp / "show.txt"
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            p.kill()
            p.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def led(self, name, brightness, max_brightness=1):
        d = self.leds / name
        d.mkdir()
        (d / "brightness").write_text(f"{brightness}\n")
        (d / "max_brightness").write_text(f"{max_brightness}\n")

    def bright(self, name):
        return (self.leds / name / "brightness").read_text().strip()

    def env(self, interval="0.2"):
        e = dict(os.environ)
        e.update(LEDS_DIR=self.leds.as_posix(), XPON_DIR=self.xpon.as_posix(), UCI_SHOW=self.show.as_posix(),
                 INTERVAL=interval, SETTLE="0", SEP="_", PATH=self.bin.as_posix() + os.pathsep + e.get("PATH", ""))
        return e

    def run_script(self, *args):
        return subprocess.run([SH, SCRIPT.as_posix(), *args], env=self.env(), capture_output=True, text=True, timeout=20)

    @data(
        (HOLD_PON_OFF, "green_pon 0"),
        (HOLD_LOS_ON, "red_los 1"),
        (HOLD_PON_OFF + HOLD_LOS_ON + LAN_NETDEV, "green_pon 0\nred_los 1"),
        (LAN_NETDEV, ""),
        (SYSTEM_BASE, ""),
        ("", ""),
        (HOLD_PON_OFF.replace("system.led_pon.sysfs='green_pon'\n", ""), ""),     # no sysfs: ignored
        (HOLD_PON_OFF.replace("hold='1'", "hold='0'"), ""),                       # hold off
        (HOLD_PON_OFF.replace("=led\n", "=other\n", 1), ""),                      # not an led section
    )
    @unpack
    def test_list(self, show, want):
        self.show.write_text(SYSTEM_BASE + show, newline="\n")
        r = self.run_script("--list")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), want)

    @data(
        # (config, leds: name -> (brightness, max), expected brightness after --once)
        (HOLD_PON_OFF, {"green_pon": (1, 1)}, {"green_pon": "0"}),
        (HOLD_LOS_ON, {"red_los": (0, 1)}, {"red_los": "1"}),
        (HOLD_LOS_ON, {"red_los": (0, 255)}, {"red_los": "255"}),
        (HOLD_PON_OFF + LAN_NETDEV, {"green_pon": (1, 1), "green_lan": (1, 1)}, {"green_pon": "0", "green_lan": "1"}),
        (HOLD_PON_OFF + HOLD_LOS_ON, {"red_los": (0, 1)}, {"red_los": "1"}),                 # green_pon missing: skipped
        (LAN_NETDEV, {"green_lan": (0, 1)}, {"green_lan": "0"}),                              # nothing held: untouched
    )
    @unpack
    def test_once(self, show, leds, want):
        self.show.write_text(SYSTEM_BASE + show, newline="\n")
        for name, (b, m) in leds.items():
            self.led(name, b, m)
        r = self.run_script("--once")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual({n: self.bright(n) for n in want}, want)

    def led_state(self, name):
        d = self.leds / name
        trig = (d / "trigger").read_text().strip() if (d / "trigger").exists() else None
        delay = (d / "delay_on").read_text().strip() if (d / "delay_on").exists() else None
        return self.bright(name), trig, delay

    @data(
        # (state, los, signal_detect, held config, expected PON (brightness, trigger, delay_on), expected LOS);
        # a blinking LED keeps the fake brightness: the kernel timer trigger toggles it on a real device
        ("down", "1", "0", "", ("0", "none", None), ("7", "timer", "500")),            # no fibre: LOS blinks 1 s
        ("down", "0", "0", "", ("0", "none", None), ("7", "timer", "500")),            # no signal also counts as dark
        ("operational", "0", "1", "", ("1", "none", None), ("0", "none", None)),       # registered: PON on
        ("registering", "0", "1", "", ("7", "timer", "250"), ("0", "none", None)),     # connecting: PON blinks 0.5 s
        ("discovery", "0", "1", "", ("7", "timer", "250"), ("0", "none", None)),
        ("down", "0", "1", "", ("0", "none", None), ("0", "none", None)),              # light but not registered
        ("unknown", "unknown", "unknown", "", ("0", "none", None), ("0", "none", None)),
        ("down", "1", "0", HOLD_PON_OFF, ("7", "keep", None), ("7", "timer", "500")),  # held PON untouched by resync
    )
    @unpack
    def test_resync(self, state, los, sig, show, pon, los_led):
        for mode in ("--resync", "--resync-later"):
            with self.subTest(mode=mode):
                self.check_resync(mode, state, los, sig, show, pon, los_led)

    def check_resync(self, mode, state, los, sig, show, pon, los_led):
        (self.xpon / "state").write_text(state + "\n")
        (self.xpon / "los").write_text(los + "\n")
        (self.xpon / "signal_detect").write_text(sig + "\n")
        self.show.write_text(SYSTEM_BASE + show, newline="\n")
        for name in ("green_pon", "red_los", "green_lan"):
            shutil.rmtree(self.leds / name, ignore_errors=True)
            self.led(name, 7)
            (self.leds / name / "trigger").write_text("keep\n")
        r = self.run_script(mode)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.led_state("green_pon"), pon)
        self.assertEqual(self.led_state("red_los"), los_led)
        self.assertEqual(self.led_state("green_lan"), ("7", "keep", None), "non-driver LEDs untouched")

    def test_exits_when_nothing_held(self):
        self.show.write_text(SYSTEM_BASE + LAN_NETDEV, newline="\n")
        r = self.run_script()
        self.assertEqual(r.returncode, 0)

    def wait_for(self, name, value, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            if self.bright(name) == value:
                return True
            time.sleep(0.1)
        return False

    @data(("green_pon", HOLD_PON_OFF, "1", "0"), ("red_los", HOLD_LOS_ON, "0", "1"))
    @unpack
    def test_loop_undoes_driver_changes(self, name, show, driver_value, held_value):
        self.show.write_text(SYSTEM_BASE + show, newline="\n")
        self.led(name, driver_value)
        self.procs.append(subprocess.Popen([SH, SCRIPT.as_posix()], env=self.env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        self.assertTrue(self.wait_for(name, held_value), "applied at start")
        (self.leds / name / "brightness").write_text(driver_value + "\n")          # the driver changes it
        self.assertTrue(self.wait_for(name, held_value), "re-applied after a driver change")

    def test_loop_reapplies_on_line_state_change(self):
        self.show.write_text(SYSTEM_BASE + HOLD_PON_OFF, newline="\n")
        self.led("green_pon", 0)
        self.procs.append(subprocess.Popen([SH, SCRIPT.as_posix()], env=self.env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        time.sleep(0.6)
        (self.xpon / "state").write_text("down\n")
        (self.leds / "green_pon" / "brightness").write_text("1\n")
        self.assertTrue(self.wait_for("green_pon", "0"))


class Files(unittest.TestCase):
    def test_shell_syntax(self):
        if not SH:
            self.skipTest("no POSIX shell")
        for f in (SCRIPT, PKG / "root/etc/init.d/led-hold", PKG / "root/etc/uci-defaults/40_luci-app-onu-system"):
            r = subprocess.run([SH, "-n", f.as_posix()], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, (f.name, r.stderr))

    def test_lf_line_endings(self):
        for f in (SCRIPT, PKG / "root/etc/init.d/led-hold", PKG / "root/etc/uci-defaults/40_luci-app-onu-system"):
            self.assertNotIn(b"\r\n", f.read_bytes(), f.name)


if __name__ == "__main__":
    unittest.main()
