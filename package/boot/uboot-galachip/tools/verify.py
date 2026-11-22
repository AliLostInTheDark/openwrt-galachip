#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail the build on unsafe sizes, configuration, ISA, relocations or SRAM use."""
import argparse
import hashlib
import json
import lzma
import re
import subprocess
from pathlib import Path

import images

def capture(prefix, tool, *args):
    return subprocess.check_output([prefix + tool, *map(str, args)], text=True)

def symbols(prefix, path):
    result = {}
    for line in capture(prefix, "nm", "-n", path).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result

FORBIDDEN = re.compile(r"\t(dmb|dsb|isb|movw|movt|ldrex|strex|clrex|wfi|wfe|ubfx|sbfx|bfi|cbn?z|it[te]*)\b")

def inspect_build(build, cross):
    report = {"isa": {}, "stack": {}, "configuration": {}, "artifacts": {}}
    paths = {"ram": build / "ram/u-boot", "production": build / "production/u-boot",
             "spl": build / "production/spl/u-boot-spl", "tpl": build / "production/tpl/u-boot-tpl"}
    syms = {}
    for name, path in paths.items():
        attrs = capture(cross, "readelf", "-A", path)
        assert "Tag_CPU_arch: v5TE" in attrs, (name, attrs)
        assert not capture(cross, "nm", "-u", path).strip(), name
        relocations = capture(cross, "readelf", "-r", path)
        types = set(re.findall(r"R_ARM_\w+", relocations))
        assert types <= {"R_ARM_RELATIVE"}, (name, types)
        if name in ("spl", "tpl"):
            assert not types, (name, types)
        disassembly = capture(cross, "objdump", "-d", "--no-show-raw-insn", path)
        forbidden = FORBIDDEN.findall(disassembly)
        assert not forbidden, (name, forbidden)
        # The ARM926 test-and-clean loops are not verified on the FA626TE.
        assert not re.search(r"mrc\s+15,\s*0,\s*pc,\s*cr7,\s*cr1[04]", disassembly)
        syms[name] = symbols(cross, path)
        report["isa"][name] = {"arch": "ARMv5TE", "relocations": sorted(types),
                                "undefined_symbols": 0}
    assert syms["production"]["_start"] == 0x40010000
    assert syms["production"]["__bss_end"] < 0x40100000
    assert syms["ram"]["_start"] == 0x41000000
    assert syms["ram"]["__bss_end"] < 0x41800000
    # SPL code and BSS from 0x40100000, its DTB after the BSS, stack and heap
    # below 0x40300000; proper is decompressed to 0x40010000-0x400fffff.
    assert syms["spl"]["_start"] == 0x40100000
    assert syms["spl"]["__bss_end"] <= 0x40107fc0
    # The TPL: code and data in the mask ROM's 0x1c00 bytes of SRAM, below its
    # metadata at 0x80001c00 and its stack at 0x80001d00-0x80002000; BSS in DDR.
    assert syms["tpl"]["_start"] == 0x80000000
    assert syms["tpl"]["__image_copy_end"] <= 0x80001c00
    assert 0x40200000 <= syms["tpl"]["__bss_start"] <= syms["tpl"]["__bss_end"] <= 0x40210000
    required = ["ENV_IS_NOWHERE", "SYS_ARM_MMU", "SPI_MEM",
                "DM_SPI", "DM_SPI_FLASH", "SYS_NS16550_MEM32", "DM_MDIO", "DM_ETH",
                "DM_ETH_PHY",
                "CMD_LOADB", "CMD_BOOTZ", "CMD_CRC32", "CMD_SLEEP",
                "HAS_CUSTOM_SYS_INIT_SP_ADDR", "LMB", "SPI_FLASH_USE_4K_SECTORS"]
    # Proper caches DDR; only the SPL manages its D-cache by hand.
    absent = ["SYS_DCACHE_OFF", "CMD_SAVEENV", "CMD_MEMTEST", "ENV_IS_IN_SPI_FLASH", "SPI_FLASH_UNLOCK_ALL",
              "SPI_FLASH_SOFT_RESET", "SPI_FLASH_SFDP_SUPPORT", "NET_RANDOM_ETHADDR"]
    commands = ["boot", "bootm", "bootz", "fwload", "zdtb", "ubcheck", "ethdiag", "tftpput", "loadx", "loady", "crc32", "sleep", "sf",
                "ping", "tftpboot", "bdinfo"]
    for name in ("ram", "production"):
        config = (build / name / ".config").read_text()
        for symbol in required:
            assert "CONFIG_" + symbol + "=y\n" in config, (name, symbol)
        for symbol in absent:
            assert "CONFIG_" + symbol + "=y\n" not in config, (name, symbol)
        assert "CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x40f00000\n" in config, name
        for command in commands:
            assert "_u_boot_list_2_cmd_2_" + command in syms[name], (name, command)
        for command in ("saveenv", "mtest", "httpd", "fpgaBoot", "multiupgrade"):
            assert "_u_boot_list_2_cmd_2_" + command not in syms[name], (name, command)
        report["configuration"][name] = "passed"
    config = (build / "production/.config").read_text()
    # Without a valid u-boot.img the SPL must fall back to YMODEM, never jump
    # into whatever the flash holds.
    for symbol in ("SPL_LEGACY_IMAGE_CRC_CHECK", "SPL_LZMA", "SPL_YMODEM_SUPPORT",
                   "TPL_YMODEM_SUPPORT", "TPL_STACK_R", "TPL_LEGACY_IMAGE_FORMAT",
                   "SPL_SYS_DCACHE_OFF"):
        assert "CONFIG_" + symbol + "=y\n" in config, symbol
    assert "CONFIG_SPL_RAW_IMAGE_SUPPORT=y\n" not in config
    # Both loaders need the real legacy parser, not spl.c's weak stub that
    # rejects every image: its header CRC message only exists in spl_legacy.c.
    for name in ("spl", "tpl"):
        data = paths[name].with_suffix(".bin").read_bytes()
        assert b"Image header CRC check failed" in data, name
    assert "CONFIG_SYS_SPI_U_BOOT_OFFS=0x%x\n" % images.UBOOT_OFFSET in config
    artifacts = build / "artifacts"
    manifest = json.loads((artifacts / "manifest.json").read_text())
    for name, expected in manifest["artifacts"].items():
        data = (artifacts / name).read_bytes()
        assert len(data) == expected["bytes"]
        assert hashlib.sha256(data).hexdigest() == expected["sha256"]
        report["artifacts"][name] = expected
    region = (artifacts / "u-boot-with-spl.bin").read_bytes()
    images.check_region(region)
    rom = (artifacts / "u-boot-tpl.bin").read_bytes()
    spl = (artifacts / "u-boot-spl.img").read_bytes()
    proper = (artifacts / "u-boot.img").read_bytes()
    assert images.rom_valid(rom) == len(rom) and region[:len(rom)] == rom
    assert rom[16:].rstrip(b"\0") == (build / "production/tpl/u-boot-tpl.bin").read_bytes().rstrip(b"\0")
    assert region[images.SPL_OFFSET:images.SPL_OFFSET + len(spl)] == spl
    assert region[images.UBOOT_OFFSET:images.UBOOT_OFFSET + len(proper)] == proper
    assert spl[64:] == (build / "production/spl/u-boot-spl.bin").read_bytes()
    raw = (build / "production/u-boot.bin").read_bytes()
    assert lzma.decompress(proper[64:]) == raw
    report["space"] = {"tpl_free": 0x1c00 - (len(rom) - 16),
                       "spl_free": images.UBOOT_OFFSET - images.SPL_OFFSET - len(spl),
                       "u-boot_free": images.REGION_SIZE - images.UBOOT_OFFSET - len(proper)}
    (artifacts / "host-inspection.json").write_text(json.dumps(report, indent=2) + "\n")
    return report

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--cross-compile", required=True)
    args = parser.parse_args()
    inspect_build(args.build, args.cross_compile)
    print("Artifact, ISA, relocation, SRAM, stack and command checks passed")
