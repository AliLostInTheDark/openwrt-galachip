#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""retire_luna_patch103.py -- prove the realtek-luna Kconfig/Makefile overlay
carries everything patches-6.18/103-realtek-luna-gpon.patch used to add, then
(only on --apply) remove that one patch file.

WHY THIS EXISTS
  Operator hard rule: driver wiring is a browsable source file under
  files-<ver>/, NEVER a patches-<ver>/*.patch -- not even for Kconfig or
  Makefile.  Patch 103 was realtek-luna's ONLY wiring: its two hunks are the
  Kconfig symbols (RTL9602C_ETH/GPON, RTL9607C_ETH/GPON) and the Makefile
  obj- + CFLAGS -O2 lines.  Those now live in
  target/linux/realtek-luna/files-6.18/drivers/net/ethernet/realtek/.

  The two mechanisms are MUTUALLY EXCLUSIVE.  files- overlays are copied into
  the kernel tree BEFORE patches are applied (include/quilt.mk:93-107), so with
  both present patch 103 applies to OUR file, its context no longer matches and
  the build fails.  Loud, which is the right direction -- but it means the
  patch must go in the same commit as the overlay.

WHY IT IS A SCRIPT AND NOT A TYPED `rm`
  Project rule for any destructive delete: write the script, list exactly what
  it will remove, state what and why, have the OPERATOR validate, then run it.
  Absolute path, one file, no wildcard, no recursion.  This script refuses to
  delete anything until it has PROVEN, against an independent reference, that
  no content is lost.

  It never touches the boot path, the rig or the board.

USAGE
  --probe    which verification routes are available here, and why not
  --check    (default) prove the overlay covers the patch.  Changes nothing
  --apply    re-run every check, then remove the ONE patch file
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

VER = "6.18"
PATCH_REL = "target/linux/realtek-luna/patches-%s/103-realtek-luna-gpon.patch" % VER
OVERLAY_REL = "target/linux/realtek-luna/files-%s/drivers/net/ethernet/realtek" % VER
KDIR = "drivers/net/ethernet/realtek"

# The ONE intended difference between the overlay and what patch 103 produces.
# Anything else is drift and must fail.
ALLOWED_ADDS = (re.compile(r'^\s*select\s+GPON_CORE\b'),)

# Independent references, most trustworthy first.  Route A re-applies the patch
# itself, so it proves the migration rather than assuming a build_dir is fresh.
ROUTES = [
    ("A:reapply", "unpatched kernel tree + patch 103 applied here, now",
     "build_dir/target-aarch64_generic_musl/linux-realtek-elnath_rtl9607f/"
     "linux-%s.31" % VER),
    ("B:prepared", "the luna build_dir's already-patched copy",
     "build_dir/target-mips_mips32_musl/linux-realtek-luna_rtl960x/linux-%s.31" % VER),
]


def strip(text, drop_allowed):
    """Comment lines and blank lines carry no build meaning; drop them so the
    comparison is about CONTENT.  Optionally drop the one allowed addition."""
    out = []
    for ln in text.splitlines():
        s = ln.strip()
        if not s or s.startswith("#"):
            continue
        if drop_allowed and any(r.match(ln) for r in ALLOWED_ADDS):
            continue
        out.append(ln.rstrip())
    return out


def find_root(start):
    d = os.path.abspath(start)
    while d != "/":
        if os.path.isdir(os.path.join(d, "target/linux")) and \
           os.path.isfile(os.path.join(d, "include/kernel.mk")):
            return d
        d = os.path.dirname(d)
    sys.exit("could not find the openwrt tree root above " + start)


def read(p):
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def route_available(root, rel):
    d = os.path.join(root, rel)
    return all(os.path.isfile(os.path.join(d, KDIR, n)) for n in ("Kconfig", "Makefile"))


def reference_files(root, route, tmp):
    """Return {name: text} for the reference this route produces, or None."""
    tag, _, rel = route
    base = os.path.join(root, rel, KDIR)
    if tag.startswith("A"):
        # elnath has ZERO patches, so its copy of a realtek file is pristine.
        # Re-apply patch 103 to it right here and compare against THAT.
        work = os.path.join(tmp, KDIR)
        os.makedirs(work, exist_ok=True)
        for n in ("Kconfig", "Makefile"):
            shutil.copy2(os.path.join(base, n), os.path.join(work, n))
        r = subprocess.run(["patch", "-p1", "--batch", "--forward",
                            "-i", os.path.join(root, PATCH_REL)],
                           cwd=tmp, capture_output=True, text=True)
        if r.returncode != 0:
            return None, "patch(1) refused: " + (r.stdout + r.stderr).strip()[:200]
        return {n: read(os.path.join(work, n)) for n in ("Kconfig", "Makefile")}, None
    return {n: read(os.path.join(base, n)) for n in ("Kconfig", "Makefile")}, None


def check(root, verbose=True):
    """Return (ok, notes). Proves the overlay loses nothing from patch 103."""
    notes = []
    patch_p = os.path.join(root, PATCH_REL)
    if not os.path.isfile(patch_p):
        notes.append("patch already retired: " + PATCH_REL)
        return True, notes

    for n in ("Kconfig", "Makefile"):
        p = os.path.join(root, OVERLAY_REL, n)
        if not os.path.isfile(p):
            notes.append("REFUSE: overlay missing %s -- deleting the patch would "
                         "delete luna's only wiring" % os.path.join(OVERLAY_REL, n))
            return False, notes

    # --- 1. every line patch 103 ADDS must survive in the overlay ------------
    ptxt = read(patch_p)
    added = [l[1:] for l in ptxt.splitlines()
             if l.startswith("+") and not l.startswith("+++")]
    overlay_all = "\n".join(read(os.path.join(root, OVERLAY_REL, n))
                            for n in ("Kconfig", "Makefile"))
    missing = [l for l in added if l.strip() and l not in overlay_all.splitlines()]
    if missing:
        notes.append("REFUSE: %d line(s) the patch adds are NOT in the overlay, "
                     "e.g. %r" % (len(missing), missing[0]))
        return False, notes
    notes.append("all %d added lines present in the overlay" % len(added))

    # --- 2. content equality against an independent reference ---------------
    avail = [r for r in ROUTES if route_available(root, r[2])]
    if not avail:
        notes.append("REFUSE: no reference available (no prepared kernel tree). "
                     "This is 'could not verify', NOT 'verified'.  Run "
                     "`make target/linux/prepare` for either target and retry.")
        return False, notes

    used = None
    for route in avail:
        tmp = tempfile.mkdtemp(prefix="retire103-")
        try:
            ref, err = reference_files(root, route, tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        if ref is None:
            notes.append("route %s unusable: %s" % (route[0], err))
            continue
        bad = []
        for n in ("Kconfig", "Makefile"):
            a = strip(ref[n], drop_allowed=False)
            b = strip(read(os.path.join(root, OVERLAY_REL, n)), drop_allowed=True)
            if a != b:
                bad.append(n)
        if bad:
            notes.append("REFUSE: overlay CONTENT differs from reference %s in %s"
                         % (route[0], ", ".join(bad)))
            return False, notes
        used = route
        notes.append("content identical to reference %s (%s); the only added "
                     "lines are `select GPON_CORE`" % (route[0], route[1]))
        break

    if used is None:
        notes.append("REFUSE: every available route failed -- 'we could not ask' "
                     "is not 'the answer is yes'")
        return False, notes
    if used[0].startswith("B"):
        notes.append("NOTE: fell back to the PREPARED copy.  It proves equality "
                     "with what that build_dir holds, not that the patch still "
                     "applies cleanly today.")

    # --- 3. the patch must touch nothing else -------------------------------
    touched = sorted(set(re.findall(r'^(?:\+\+\+|---)\s+[ab]/(\S+)', ptxt, re.M)))
    extra = [t for t in touched if t not in (KDIR + "/Kconfig", KDIR + "/Makefile")]
    if extra:
        notes.append("REFUSE: patch also touches %s -- not covered by this "
                     "migration" % ", ".join(extra))
        return False, notes
    notes.append("patch touches only %s" % ", ".join(touched))
    return True, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=None)
    ap.add_argument("--probe", action="store_true",
                    help="report which verification routes exist here")
    ap.add_argument("--apply", action="store_true",
                    help="remove the patch (only after every check passes)")
    ap.add_argument("--LABEL", default=None)
    a = ap.parse_args()

    root = a.root or find_root(os.path.dirname(os.path.abspath(__file__)))
    print("retire luna patch 103   root=%s%s" %
          (root, "   LABEL=%s" % a.LABEL if a.LABEL else ""))

    if a.probe:
        for tag, why, rel in ROUTES:
            ok = route_available(root, rel)
            print("  %-11s %-58s %s" % (tag, why, "AVAILABLE" if ok else "absent"))
        return 0

    ok, notes = check(root)
    for n in notes:
        print("  " + n)

    target = os.path.join(root, PATCH_REL)
    if not a.apply:
        print("\nWOULD REMOVE exactly one file (nothing removed -- this is --check):")
        if os.path.isfile(target):
            st = os.stat(target)
            print("  %s  (%d bytes)" % (target, st.st_size))
            print("  recoverable: it is committed to git, so `git checkout -- %s`"
                  % PATCH_REL)
        print("\n%s" % ("VERIFIED: safe to --apply" if ok
                        else "REFUSED: do NOT apply, see above"))
        return 0 if ok else 1

    if not ok:
        print("\nREFUSED: checks did not pass, nothing removed")
        return 1
    if not os.path.isfile(target):
        print("\nnothing to do")
        return 0
    if os.path.islink(target) or not os.path.isfile(target):
        print("\nREFUSED: %s is not a regular file" % target)
        return 1
    os.remove(target)          # one literal path, no wildcard, no recursion
    print("\nREMOVED %s" % target)
    print("Now re-run: target/linux/gpon-common/build_wiring_guard.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
