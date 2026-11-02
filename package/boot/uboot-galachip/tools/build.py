#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build bootloader components only; no firmware build or device access."""
import argparse
import hashlib
import os
import shutil
import subprocess
import tarfile
from pathlib import Path
from images import ARCHIVE_SHA256, compress, package
from verify import inspect_build

PACKAGE = Path(__file__).resolve().parents[1]

TOPDIR = PACKAGE.parents[2]

def openwrt_revision():
    """OpenWrt's scripts/getver.sh revision, e.g. r37201+20-6a8098fbee."""
    try:
        return subprocess.run([TOPDIR / "scripts/getver.sh"], cwd=TOPDIR, check=True,
                              capture_output=True, text=True).stdout.strip() or "unknown"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"

def source_date_epoch():
    """The last commit's time, as OpenWrt derives SOURCE_DATE_EPOCH."""
    try:
        return subprocess.run(["git", "log", "-1", "--format=%ct"], cwd=TOPDIR, check=True,
                              capture_output=True, text=True).stdout.strip() or "0"
    except (OSError, subprocess.CalledProcessError):
        return "0"

def run(args, **kwargs):
    subprocess.run([str(v) for v in args], check=True, **kwargs)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--archive", type=Path)
    p.add_argument("--source", type=Path, help="source prepared by OpenWrt with pinned hash and patches")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cross-compile", required=True)
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--revision", default="local-uncommitted")
    p.add_argument("--localversion",
                   help="appended to the version, as OpenWrt does: -OpenWrt-<REVISION>")
    a = p.parse_args()
    if a.jobs < 1:
        p.error("--jobs must be positive")
    if bool(a.archive) == bool(a.source):
        p.error("provide exactly one of --archive or --source")
    if a.archive and hashlib.sha256(a.archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        p.error("upstream archive SHA256 mismatch")
    output = a.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = a.source.resolve() if a.source else output / "u-boot-v2026.10"
    patches = sorted((PACKAGE / "patches").glob("*.patch"))
    patch_hash = hashlib.sha256(b"".join(p.read_bytes() for p in patches)).hexdigest()
    prepared = ARCHIVE_SHA256 + ":" + patch_hash + "\n"
    if not source.exists() and a.archive:
        with tarfile.open(a.archive) as archive:
            archive.extractall(output, filter="data")
        for patch in patches:
            with patch.open() as patch_input:
                run(["patch", "-p1", "--batch"], cwd=source, stdin=patch_input)
        (source / ".g7-prepared").write_text(prepared)
    elif not a.source and (not (source / ".g7-prepared").exists() or
                           (source / ".g7-prepared").read_text() != prepared):
        p.error("source/patch identity changed; choose a fresh output directory")
    if a.source and not (source / "board/tplink/xz000-g7/Kconfig").exists():
        p.error("--source must be prepared by the OpenWrt package")
    shutil.copytree(PACKAGE / "files", source, dirs_exist_ok=True)
    localversion = a.localversion or "-OpenWrt-" + openwrt_revision()
    epoch = os.environ.get("SOURCE_DATE_EPOCH") or source_date_epoch()
    env = dict(os.environ, SOURCE_DATE_EPOCH=epoch,
               KBUILD_BUILD_USER="openwrt", KBUILD_BUILD_HOST="g7-builder")
    builds = {}
    for variant, config in (("ram", "xz000_g7_ram"), ("production", "xz000_g7")):
        build = output / variant
        command = ["make", "-C", source, "O=" + str(build),
                   "CROSS_COMPILE=" + a.cross_compile, "LOCALVERSION=" + localversion]
        run(command + [config + "_defconfig"], env=env)
        run(command + ["-j" + str(a.jobs)], env=env)
        builds[variant] = build / "u-boot.bin"
    stage_dir = output / "stages"
    stage_dir.mkdir(exist_ok=True)
    packed, header = compress(builds["production"].read_bytes())
    (stage_dir / "proper.lzma").write_bytes(packed)
    (stage_dir / "meta.bin").write_bytes(header)
    run(["make", "-f", PACKAGE / "stages/Makefile",
         "UBOOT_SRC=" + str(source), "CROSS_COMPILE=" + a.cross_compile,
         "-j" + str(a.jobs)], cwd=stage_dir, env=env)
    package(stage_dir, builds["ram"], builds["production"],
            output / "artifacts", a.revision, int(epoch))
    inspect_build(output, a.cross_compile)
    print("RAM-only artifacts:", output / "artifacts")

if __name__ == "__main__":
    main()
