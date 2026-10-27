#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checked GC1601 image construction. Never opens a device or writes flash."""
import argparse
import hashlib
import json
import lzma
import struct
import zlib
from pathlib import Path

STAGE_SIZE = 0x1e000
SPL_LIMIT = 0x1c00
PROPER_MAX = 0xe0000
ARCHIVE_SHA256 = "3f856da3cbbc93af51ff95fde18588a348b4a26723b01d9841e999d988481015"

def crc(data):
    return zlib.crc32(data) & 0xffffffff

def words_with_crc(words):
    body = struct.pack("<" + "I" * len(words), *words)
    return body + struct.pack("<I", crc(body))

def compress(raw):
    if not raw or len(raw) > PROPER_MAX:
        raise ValueError("U-Boot proper exceeds its disjoint output allocation")
    packed = lzma.compress(raw, format=lzma.FORMAT_ALONE, filters=[{
        "id": lzma.FILTER_LZMA1, "dict_size": 1 << 20,
        "lc": 3, "lp": 0, "pb": 2, "mode": lzma.MODE_NORMAL,
        "nice_len": 273, "mf": lzma.MF_BT4,
    }])
    packed = packed[:5] + struct.pack("<Q", len(raw)) + packed[13:]
    header = words_with_crc([
        0x315a3747, len(packed), len(raw), crc(packed), crc(raw),
        0x40010000, 0x40010000,
    ])
    if lzma.decompress(packed, format=lzma.FORMAT_ALONE) != raw:
        raise ValueError("LZMA round trip failed")
    return packed, header

def proper_image(wrapper):
    if len(wrapper) < 64 or len(wrapper) > STAGE_SIZE - 32:
        raise ValueError("wrapper, metadata and payload exceed 0x1e000")
    footer = words_with_crc([0x31503747, 1, len(wrapper), 0x40100000,
                             crc(wrapper), 0, 0])
    return wrapper + b"\xff" * (STAGE_SIZE - 32 - len(wrapper)) + footer

def rom_image(spl):
    spl += b"\0" * (-len(spl) % 4)
    if not spl or len(spl) > SPL_LIMIT:
        raise ValueError("SPL payload exceeds ROM 0x1c00 limit")
    checksum = sum(struct.unpack("<" + "I" * (len(spl) // 4), spl)) & 0xffffffff
    return struct.pack("<4I", 0x03020100, len(spl), 0, checksum) + spl

def boot_region(rom, proper):
    if len(rom) > 0x2000 or len(proper) != STAGE_SIZE:
        raise ValueError("invalid boot allocations")
    return rom + b"\xff" * (0x2000 - len(rom)) + proper

def uart_image(raw):
    if not raw or len(raw) > PROPER_MAX:
        raise ValueError("RAM image exceeds UART recovery allocation")
    package = struct.pack("<4I", 0x31523747, len(raw), 0x41000000, crc(raw)) + raw
    return package + b"\x1a" * (-len(package) % 128)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def package(stage_dir, ram, production, output, revision):
    output.mkdir(parents=True, exist_ok=True)
    proper = proper_image((stage_dir / "wrapper.bin").read_bytes())
    spl = rom_image((stage_dir / "spl.bin").read_bytes())
    artifacts = {
        "xz000-g7-uboot-ram.bin": ram.read_bytes(),
        "xz000-g7-uboot-uart.pkg": uart_image(ram.read_bytes()),
        "xz000-g7-uboot-proper.img": proper,
        "xz000-g7-spl-rom.bin": spl,
        "xz000-g7-bootregion.bin": boot_region(spl, proper),
    }
    manifest = {
        "upstream": "v2026.10", "source_sha256": ARCHIVE_SHA256,
        "port_revision": revision, "hardware_status": "UNTESTED; RAM-only",
        "rom_recovery": "not demonstrated; installation blocked",
        "source_date_epoch": 1791158400,
        "addresses": {"ram_entry": "0x41000000", "wrapper": "0x40100000",
                      "proper": "0x40010000", "spl": "0x80000000"},
        "limits": {"spl_payload": SPL_LIMIT, "proper_expanded": PROPER_MAX,
                   "compressed_allocation": STAGE_SIZE,
                   "dma": ["0x40800000", "0x40912000"],
                   "decoder_heap": ["0x40200000", "0x40210000"],
                   "wrapper_stack": ["0x40300000", "0x40304000"],
                   "spl_bss_end": "0x80001c00",
                   "rom_metadata": ["0x80001c00", "0x80001d00"],
                   "spl_stack": ["0x80001d00", "0x80002000"]},
        "configurations": {},
        "sizes": {"proper_raw": production.stat().st_size,
                  "wrapper_raw": (stage_dir / "wrapper.bin").stat().st_size,
                  "compressed": (stage_dir / "proper.lzma").stat().st_size},
        "artifacts": {},
    }
    for name, data in artifacts.items():
        path = output / name
        path.write_bytes(data)
        manifest["artifacts"][name] = {"bytes": len(data), "sha256": digest(path)}
    for variant, image in (("ram", ram), ("production", production)):
        config = image.parent / ".config"
        manifest["configurations"][variant] = {"sha256": digest(config)}
        (output / (variant + ".config")).write_bytes(config.read_bytes())
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("compress")
    c.add_argument("proper", type=Path)
    c.add_argument("stage_dir", type=Path)
    p = sub.add_parser("package")
    p.add_argument("stage_dir", type=Path)
    p.add_argument("ram", type=Path)
    p.add_argument("production", type=Path)
    p.add_argument("output", type=Path)
    p.add_argument("--revision", required=True)
    args = parser.parse_args()
    if args.command == "compress":
        packed, header = compress(args.proper.read_bytes())
        args.stage_dir.mkdir(parents=True, exist_ok=True)
        (args.stage_dir / "proper.lzma").write_bytes(packed)
        (args.stage_dir / "meta.bin").write_bytes(header)
    else:
        package(args.stage_dir, args.ram, args.production, args.output, args.revision)

if __name__ == "__main__":
    main()
