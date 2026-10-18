#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Wrap a raw loader image in the GC1601 mask ROM header.

    mkspl.py loader.bin loader-flash.bin

The ROM reads 16 bytes at flash offset 0: magic 0x03020100, image size,
0, and the 32-bit sum of the image words. The image follows at offset 0x10,
at most 0x1c00 bytes.
"""
import struct
import sys

MAGIC = 0x03020100
MAX_SIZE = 0x1c00


def main(src, dst):
    image = open(src, "rb").read()
    image += b"\0" * (-len(image) % 4)
    if len(image) > MAX_SIZE:
        sys.exit(f"{src}: {len(image):#x} bytes, the mask ROM loads at most {MAX_SIZE:#x}")
    checksum = sum(struct.unpack(f"<{len(image) // 4}I", image)) & 0xffffffff
    header = struct.pack("<4I", MAGIC, len(image), 0, checksum)
    open(dst, "wb").write(header + image)
    print(f"{dst}: {len(image):#x} bytes, checksum {checksum:#010x}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
