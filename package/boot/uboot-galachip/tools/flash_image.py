#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a full-chip image for an external SPI programmer from a verified
backup. Only the boot region (0x0-0x20000) changes. Never opens a device or
writes flash."""
import argparse
import hashlib
import struct
from pathlib import Path

from images import REGION_SIZE, check_region

CHIP_SIZE = 0x400000
FIRMWARE_MAGIC = 0x03000003

def check_backup(backup):
    if len(backup) != CHIP_SIZE:
        raise ValueError("backup must be a complete 4 MiB chip read")
    if struct.unpack("<I", backup[0x20000:0x20004])[0] != FIRMWARE_MAGIC:
        raise ValueError("backup has no firmware tag at 0x20000")
    if backup[0x3ef100:0x3ef106] in (b"\xff" * 6, b"\0" * 6):
        raise ValueError("backup has no factory MAC at 0x3ef100")

def build(backup, region):
    check_backup(backup)
    check_region(region)
    image = region + backup[REGION_SIZE:]
    assert len(image) == CHIP_SIZE
    return image

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path, help="verified 4 MiB chip read")
    parser.add_argument("region", type=Path, help="u-boot-with-spl.bin")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("refusing to overwrite %s" % args.output)
    image = build(args.backup.read_bytes(), args.region.read_bytes())
    args.output.write_bytes(image)
    print("changed 0x0-0x%x" % REGION_SIZE)
    print("sha256 %s" % hashlib.sha256(image).hexdigest())

if __name__ == "__main__":
    main()
