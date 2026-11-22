#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""GC1601 boot region assembly and checks. Never opens a device or writes flash.

Boot region (NOR 0x0-0x20000): the U-Boot TPL with its mask ROM header at 0,
the U-Boot SPL as a legacy image at 0x2000 and the LZMA-compressed U-Boot as
a legacy image (u-boot.img) at 0xa000.
"""
import hashlib
import json
import lzma
import struct
import zlib

ARCHIVE_SHA256 = "3f856da3cbbc93af51ff95fde18588a348b4a26723b01d9841e999d988481015"
ROM_PAYLOAD_MAX = 0x1c00
SPL_OFFSET = 0x2000
UBOOT_OFFSET = 0xa000
REGION_SIZE = 0x20000
SPL_LOAD = 0x40100000
UBOOT_LOAD = 0x40010000
PROPER_MAX = 0xe0000

IH_MAGIC = 0x27051956
IH_COMP = {"none": 0, "lzma": 3}

def crc(data):
    return zlib.crc32(data) & 0xffffffff

def lzma_alone(raw):
    """LZMA-alone stream with the real size, as U-Boot's lzma decoder wants it."""
    if not raw or len(raw) > PROPER_MAX:
        raise ValueError("U-Boot proper exceeds 0x%x bytes" % PROPER_MAX)
    packed = lzma.compress(raw, format=lzma.FORMAT_ALONE, filters=[{
        "id": lzma.FILTER_LZMA1, "dict_size": 1 << 20,
        "lc": 3, "lp": 0, "pb": 2, "mode": lzma.MODE_NORMAL,
        "nice_len": 273, "mf": lzma.MF_BT4,
    }])
    packed = packed[:5] + struct.pack("<Q", len(raw)) + packed[13:]
    if lzma.decompress(packed, format=lzma.FORMAT_ALONE) != raw:
        raise ValueError("LZMA round trip failed")
    return packed

def legacy_image(image, load, comp, limit):
    """Check a mkimage legacy image as U-Boot does; return its total length."""
    if len(image) < 64:
        raise ValueError("legacy image is truncated")
    magic, hcrc, _, size, ih_load, ep, dcrc = struct.unpack(">7I", image[:28])
    _, arch, kind, ih_comp = image[28:32]
    if magic != IH_MAGIC or crc(image[:4] + bytes(4) + image[8:64]) != hcrc:
        raise ValueError("legacy image header is invalid")
    if (ih_load, ep, arch, kind, ih_comp) != (load, load, 2, 5, IH_COMP[comp]):
        raise ValueError("legacy image is not an ARM firmware image for 0x%x" % load)
    if not size or 64 + size > min(limit, len(image)) or crc(image[64:64 + size]) != dcrc:
        raise ValueError("legacy image data is invalid")
    return 64 + size

def rom_image(payload):
    payload += b"\0" * (-len(payload) % 4)
    if not payload or len(payload) > ROM_PAYLOAD_MAX:
        raise ValueError("TPL exceeds the ROM's 0x1c00 limit")
    checksum = sum(struct.unpack("<%dI" % (len(payload) // 4), payload)) & 0xffffffff
    return struct.pack("<4I", 0x03020100, len(payload), 0, checksum) + payload

def rom_valid(image):
    """The mask ROM's checks: magic, size and the payload's 32-bit word sum."""
    if len(image) < 20:
        raise ValueError("ROM image is truncated")
    magic, length, _, checksum = struct.unpack("<4I", image[:16])
    payload = image[16:16 + length]
    if magic != 0x03020100 or not length or length > ROM_PAYLOAD_MAX or \
       length % 4 or len(payload) != length:
        raise ValueError("ROM header is invalid")
    if sum(struct.unpack("<%dI" % (length // 4), payload)) & 0xffffffff != checksum:
        raise ValueError("ROM checksum mismatch")
    return 16 + length

def check_region(region):
    if len(region) != REGION_SIZE:
        raise ValueError("boot region must be exactly 0x%x bytes" % REGION_SIZE)
    rom_valid(region[:SPL_OFFSET])
    legacy_image(region[SPL_OFFSET:UBOOT_OFFSET], SPL_LOAD, "none", UBOOT_OFFSET - SPL_OFFSET)
    legacy_image(region[UBOOT_OFFSET:], UBOOT_LOAD, "lzma", REGION_SIZE - UBOOT_OFFSET)

def boot_region(rom, spl_img, uboot_img):
    if len(rom) > SPL_OFFSET or len(spl_img) > UBOOT_OFFSET - SPL_OFFSET or \
       len(uboot_img) > REGION_SIZE - UBOOT_OFFSET:
        raise ValueError("boot region allocations exceeded")
    region = (rom.ljust(SPL_OFFSET, b"\xff") + spl_img.ljust(UBOOT_OFFSET - SPL_OFFSET, b"\xff") +
              uboot_img.ljust(REGION_SIZE - UBOOT_OFFSET, b"\xff"))
    check_region(region)
    return region

def digest(data):
    return hashlib.sha256(data).hexdigest()

def package(output, artifacts, configs, revision, epoch):
    output.mkdir(parents=True, exist_ok=True)
    manifest = {
        "upstream": "v2026.10", "source_sha256": ARCHIVE_SHA256,
        "port_revision": revision, "source_date_epoch": epoch,
        "layout": {"tpl": "0x0", "spl": hex(SPL_OFFSET), "u-boot": hex(UBOOT_OFFSET),
                   "region": hex(REGION_SIZE)},
        "addresses": {"ram_entry": "0x41000000", "spl": hex(SPL_LOAD),
                      "proper": hex(UBOOT_LOAD), "tpl": "0x80000000"},
        "configurations": {name: digest(path.read_bytes()) for name, path in configs.items()},
        "artifacts": {},
    }
    for name, data in artifacts.items():
        (output / name).write_bytes(data)
        manifest["artifacts"][name] = {"bytes": len(data), "sha256": digest(data)}
    for name, path in configs.items():
        (output / (name + ".config")).write_bytes(path.read_bytes())
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
