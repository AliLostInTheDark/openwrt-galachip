#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a full-chip image for an external SPI programmer from a verified
backup. Only the proper allocation (and SPL, if given) changes. Never opens a
device or writes flash."""
import argparse
import hashlib
import struct
from pathlib import Path

from images import STAGE_SIZE, SPL_LIMIT, crc

CHIP_SIZE = 0x400000
PROPER_OFFSET = 0x2000
FIRMWARE_MAGIC = 0x03000003

def check_proper(proper):
    if len(proper) != STAGE_SIZE:
        raise ValueError("proper.img must be exactly 0x1e000 bytes")
    footer = proper[-32:]
    words = struct.unpack("<8I", footer)
    if crc(footer[:28]) != words[7]:
        raise ValueError("proper.img footer CRC mismatch")
    magic, version, length, entry, wrapper_crc = words[:5]
    if magic != 0x31503747 or version != 1 or entry != 0x40100000:
        raise ValueError("proper.img footer is not a G7 wrapper footer")
    if not 64 <= length <= STAGE_SIZE - 32 or crc(proper[:length]) != wrapper_crc:
        raise ValueError("proper.img wrapper CRC mismatch")

def check_spl(spl):
    if len(spl) < 20 or len(spl) > 0x2000:
        raise ValueError("SPL ROM image size is invalid")
    magic, length, _, checksum = struct.unpack("<4I", spl[:16])
    payload = spl[16:16 + length]
    if magic != 0x03020100 or not length or length > SPL_LIMIT or \
       length % 4 or len(payload) != length:
        raise ValueError("SPL ROM header is invalid")
    if sum(struct.unpack("<%dI" % (length // 4), payload)) & 0xffffffff != checksum:
        raise ValueError("SPL ROM checksum mismatch")

def check_backup(backup):
    if len(backup) != CHIP_SIZE:
        raise ValueError("backup must be a complete 4 MiB chip read")
    if struct.unpack("<I", backup[0x20000:0x20004])[0] != FIRMWARE_MAGIC:
        raise ValueError("backup has no firmware tag at 0x20000")
    if backup[0x3ef100:0x3ef106] in (b"\xff" * 6, b"\0" * 6):
        raise ValueError("backup has no factory MAC at 0x3ef100")

def build(backup, proper, spl=None):
    check_backup(backup)
    check_proper(proper)
    image = bytearray(backup)
    image[PROPER_OFFSET:PROPER_OFFSET + STAGE_SIZE] = proper
    start = PROPER_OFFSET
    if spl is not None:
        check_spl(spl)
        image[:PROPER_OFFSET] = spl + b"\xff" * (PROPER_OFFSET - len(spl))
        start = 0
    end = PROPER_OFFSET + STAGE_SIZE
    if image[:start] != backup[:start] or image[end:] != backup[end:]:
        raise ValueError("bytes outside the boot region changed")
    return bytes(image)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path, help="verified 4 MiB chip read")
    parser.add_argument("proper", type=Path, help="xz000-g7-uboot-proper.img")
    parser.add_argument("output", type=Path)
    parser.add_argument("--spl", type=Path, help="xz000-g7-spl-rom.bin (last step only)")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("refusing to overwrite %s" % args.output)
    spl = args.spl.read_bytes() if args.spl else None
    image = build(args.backup.read_bytes(), args.proper.read_bytes(), spl)
    args.output.write_bytes(image)
    print("changed 0x%x-0x%x" % (0 if spl else PROPER_OFFSET, PROPER_OFFSET + STAGE_SIZE))
    print("sha256 %s" % hashlib.sha256(image).hexdigest())

if __name__ == "__main__":
    main()
