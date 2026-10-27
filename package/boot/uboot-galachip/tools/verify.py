#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fail the build on unsafe sizes, configuration, ISA, relocations or SRAM use."""
import argparse
import hashlib
import json
import lzma
import re
import struct
import subprocess
import zlib
from pathlib import Path

def capture(prefix, tool, *args):
    return subprocess.check_output([prefix + tool, *map(str, args)], text=True)

def symbols(prefix, path):
    result = {}
    for line in capture(prefix, "nm", "-n", path).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result

def inspect_build(build, cross):
    report = {"isa": {}, "stack": {}, "configuration": {}, "artifacts": {}}
    paths = {"ram": build / "ram/u-boot", "production": build / "production/u-boot",
             "wrapper": build / "stages/wrapper.elf", "spl": build / "stages/spl.elf"}
    syms = {}
    for name, path in paths.items():
        attrs = capture(cross, "readelf", "-A", path)
        assert "Tag_CPU_arch: v5TE" in attrs, (name, attrs)
        assert not capture(cross, "nm", "-u", path).strip(), name
        relocations = capture(cross, "readelf", "-r", path)
        types = set(re.findall(r"R_ARM_\w+", relocations))
        assert types <= {"R_ARM_RELATIVE"}, (name, types)
        if name in ("spl", "wrapper"):
            assert not types, (name, types)
        disassembly = capture(cross, "objdump", "-d", "--no-show-raw-insn", path)
        forbidden = re.findall(r"\t(dmb|dsb|isb|movw|movt|ldrex|strex|clrex|wfi|wfe|ubfx|sbfx|bfi)\b",
                               disassembly)
        assert not forbidden, (name, forbidden)
        assert not re.search(r"mrc\s+15,\s*0,\s*pc,\s*cr7,\s*cr1[04]", disassembly)
        syms[name] = symbols(cross, path)
        report["isa"][name] = {"arch": "ARMv5TE", "relocations": sorted(types),
                                "undefined_symbols": 0}
    assert syms["production"]["_start"] == 0x40010000
    assert syms["production"]["__bss_end"] < 0x40100000
    assert syms["ram"]["_start"] == 0x41000000
    assert syms["ram"]["__bss_end"] < 0x41800000
    assert syms["spl"]["__bss_end"] <= 0x80001c00
    assert syms["spl"]["__stack_bottom"] == 0x80001d00
    assert syms["spl"]["__stack_top"] == 0x80002000
    for name in ("spl", "wrapper"):
        frames = {}
        for su in (build / "stages").glob("*.su"):
            for line in su.read_text().splitlines():
                location, size, kind = line.split("\t")
                function = location.rsplit(":", 1)[-1]
                if function in syms[name]:
                    assert kind == "static", (name, function, kind)
                    frames[function] = int(size)
        # Sum of all reachable frames is conservative for this nonrecursive C.
        # Include 256 bytes for assembly/libgcc and alignment not covered by .su.
        bound = sum(frames.values()) + 256
        budget = 0x300 if name == "spl" else 0x4000
        assert bound < budget, (name, bound, budget)
        report["stack"][name] = {"conservative_bound": bound, "budget": budget,
                                 "frames": frames,
                                 "bss_end": hex(syms[name]["__bss_end"])}
    required = ["ENV_IS_NOWHERE", "SYS_ICACHE_OFF", "SYS_DCACHE_OFF", "SPI_MEM",
                "DM_SPI", "DM_SPI_FLASH", "SYS_NS16550_MEM32", "DM_MDIO", "DM_ETH",
                "DM_ETH_PHY",
                "CMD_LOADB", "CMD_BOOTZ", "CMD_CRC32", "CMD_SLEEP",
                "HAS_CUSTOM_SYS_INIT_SP_ADDR", "LMB"]
    absent = ["CMD_SAVEENV", "CMD_MEMTEST", "ENV_IS_IN_SPI_FLASH", "SPI_FLASH_UNLOCK_ALL",
              "SPI_FLASH_SOFT_RESET", "SPI_FLASH_SFDP_SUPPORT", "NET_RANDOM_ETHADDR"]
    commands = ["bootz", "g7boot", "g7diag", "loadx", "loady", "crc32", "sleep", "sf",
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
    artifacts = build / "artifacts"
    manifest = json.loads((artifacts / "manifest.json").read_text())
    for name, expected in manifest["artifacts"].items():
        data = (artifacts / name).read_bytes()
        assert len(data) == expected["bytes"]
        assert hashlib.sha256(data).hexdigest() == expected["sha256"]
        report["artifacts"][name] = expected
    spl = (artifacts / "xz000-g7-spl-rom.bin").read_bytes()
    magic, length, zero, checksum = struct.unpack("<4I", spl[:16])
    assert magic == 0x03020100 and zero == 0 and 0 < length <= 0x1c00
    assert length == len(spl) - 16
    assert sum(struct.unpack("<" + "I" * (length // 4), spl[16:])) & 0xffffffff == checksum
    proper = (artifacts / "xz000-g7-uboot-proper.img").read_bytes()
    assert syms["wrapper"]["image_header"] == 0x40100020
    header = struct.unpack("<8I", proper[32:64])
    assert header[0] == 0x315a3747 and header[5:7] == (0x40010000, 0x40010000)
    assert zlib.crc32(proper[32:60]) == header[7]
    begin = syms["wrapper"]["compressed_start"] - 0x40100000
    end = syms["wrapper"]["compressed_end"] - 0x40100000
    packed = proper[begin:end]
    raw = (build / "production/u-boot.bin").read_bytes()
    assert len(packed) == header[1] and len(raw) == header[2]
    assert zlib.crc32(packed) == header[3] and zlib.crc32(raw) == header[4]
    assert lzma.decompress(packed) == raw
    footer = struct.unpack("<8I", proper[-32:])
    assert footer[0:2] == (0x31503747, 1) and 64 <= footer[2] <= 0x1dfe0
    assert footer[3] == 0x40100000 and footer[5:7] == (0, 0)
    assert zlib.crc32(proper[:footer[2]]) == footer[4]
    assert zlib.crc32(proper[-32:-4]) == footer[7]
    combined = (artifacts / "xz000-g7-bootregion.bin").read_bytes()
    assert len(combined) == 0x20000 and len(proper) == 0x1e000
    assert combined[:len(spl)] == spl and combined[0x2000:] == proper
    (artifacts / "host-inspection.json").write_text(json.dumps(report, indent=2) + "\n")
    return report

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--cross-compile", required=True)
    args = parser.parse_args()
    inspect_build(args.build, args.cross_compile)
    print("Artifact, ISA, relocation, SRAM, stack and command checks passed")
