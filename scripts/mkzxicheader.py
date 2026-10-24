#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Generates the 320-byte (0x140) vendor header required by stock U-Boot
# on ZTE Sanechips ZX279125 platforms (e.g. VSOL V2801SG).
#
# Author: Ifran Ali <khanu.i.i.k.2000@gmail.com>

import sys
import struct
import zlib
import os

HEADER_SIZE = 320
BLOCKSIZE = 65536
IH_MAGIC = 0x27051956
MAGIC_WORDS = (0x99999999, 0x44444444, 0x55555555, 0xaaaaaaaa)
VERSION_STR = b'V6.1.2P1T8'
MODEL_STR = b'ZXIC DG401 NULL V6.1.2P1T8 bosaV1.3'
BOARD_STR = b'V2.8S'
HWID_STR = b'\x15V$VZX01D401'
FLASH_START_ADDR = 0x40000
# With a loader, stock U-Boot boots the loader in the kernel slot and the
# loader boots the real kernel from this offset.
LOADER_SLOT = 0x30000

def uimage_len(data, what):
    ih_magic, = struct.unpack('>I', data[:4])
    if ih_magic != IH_MAGIC:
        raise ValueError(f"{what} does not start with uImage magic (got 0x{ih_magic:08x}, expected 0x{IH_MAGIC:08x})")
    ih_size, = struct.unpack('>I', data[12:16])
    if 64 + ih_size > len(data):
        raise ValueError(f"Truncated {what} uImage")
    return 64 + ih_size

def create_image(payload, loader=None):
    k_len = uimage_len(payload, "Input")

    # append-kernel | pad-to BLOCKSIZE places SquashFS at this boundary.
    pos = ((k_len + BLOCKSIZE - 1) // BLOCKSIZE) * BLOCKSIZE
    if payload[pos:pos + 4] != b'hsqs' or len(payload) < pos + 96:
        raise ValueError("Missing SquashFS superblock after aligned kernel")
    rootfs_bytes = payload[pos:]
    bytes_used, = struct.unpack_from('<Q', rootfs_bytes, 40)
    if bytes_used < 96 or bytes_used > len(rootfs_bytes):
        raise ValueError("Invalid SquashFS bytes_used")
    # The overlay starts at the next flash erase block after the SquashFS
    # (4 KiB on this NOR), so only its own bytes are stable for stock's CRC.
    r_len = bytes_used

    if loader:
        k_len = uimage_len(loader, "Loader")
        if HEADER_SIZE + k_len > LOADER_SLOT:
            raise ValueError(f"Loader does not fit its 0x{LOADER_SLOT:x} slot")
        uimage_bytes = loader[:k_len]
        k_padding = b'\xff' * (LOADER_SLOT - HEADER_SIZE - k_len) + payload[:pos]
        k_alloc = LOADER_SLOT + pos
    else:
        # 64 KiB flash alignment for rootfs:
        # 0x40000 (Flash start) + Header (320 bytes) + uImage (k_len) + padding = r_off (aligned to 64k)
        uimage_bytes = payload[:k_len]
        k_alloc = ((HEADER_SIZE + k_len + BLOCKSIZE - 1) // BLOCKSIZE) * BLOCKSIZE
        k_padding = b'\xff' * (k_alloc - HEADER_SIZE - k_len)

    r_off = FLASH_START_ADDR + k_alloc
    tot_len = k_alloc + len(rootfs_bytes)

    k_crc = zlib.crc32(uimage_bytes)
    r_crc = zlib.crc32(rootfs_bytes[:r_len])

    hdr = bytearray(HEADER_SIZE)

    # 0x00 - 0x0f: Magic identification words
    struct.pack_into('<4I', hdr, 0x00, *MAGIC_WORDS)

    # 0x24: Version string
    hdr[0x24 : 0x24 + len(VERSION_STR)] = VERSION_STR

    struct.pack_into('<I', hdr, 0x50, 1)
    struct.pack_into('<I', hdr, 0x54, tot_len)
    struct.pack_into('<I', hdr, 0x58, k_len)
    struct.pack_into('<I', hdr, 0x5c, HEADER_SIZE)
    struct.pack_into('<I', hdr, 0x60, k_crc)
    struct.pack_into('<I', hdr, 0x64, r_len)
    struct.pack_into('<I', hdr, 0x68, tot_len - HEADER_SIZE)
    struct.pack_into('<I', hdr, 0x6c, r_crc)
    struct.pack_into('<I', hdr, 0x70, FLASH_START_ADDR)
    struct.pack_into('<I', hdr, 0x74, k_alloc)
    struct.pack_into('<I', hdr, 0x78, r_off)
    struct.pack_into('<I', hdr, 0x7c, 0x780000 - r_off)
    struct.pack_into('<I', hdr, 0x80, 0x3e0000)
    struct.pack_into('<I', hdr, 0x84, k_alloc)
    struct.pack_into('<I', hdr, 0x88, 0x510000)
    struct.pack_into('<I', hdr, 0x8c, 0x270000)

    # 0x90: Model string
    hdr[0x90 : 0x90 + len(MODEL_STR)] = MODEL_STR
    struct.pack_into('<I', hdr, 0xc0, 1)

    # 0x100: Board string and HWID
    hdr[0x100 : 0x100 + len(BOARD_STR)] = BOARD_STR
    hdr[0x110 : 0x110 + len(HWID_STR)] = HWID_STR

    # 0x120 - 0x13f: Trailing padding
    for i in range(0x120, 0x140):
        hdr[i] = 0xff

    # 0xd8: Header CRC32 computed across bytes 0x14 to 0xd7 (196 bytes)
    hdr_crc = zlib.crc32(hdr[0x14 : 0x14 + 196])
    struct.pack_into('<I', hdr, 0xd8, hdr_crc)

    return bytes(hdr) + uimage_bytes + k_padding + rootfs_bytes

def verify_image(path):
    if not os.path.exists(path):
        print(f"Error: file not found: {path}", file=sys.stderr)
        return 1

    with open(path, 'rb') as f:
        hdr = f.read(HEADER_SIZE)
        if len(hdr) < HEADER_SIZE:
            print("Error: file smaller than 320-byte header", file=sys.stderr)
            return 1

        magic = struct.unpack('<4I', hdr[:16])
        if magic != MAGIC_WORDS:
            print(f"Error: invalid header magic: {[hex(x) for x in magic]}", file=sys.stderr)
            return 1

        hdr_crc = struct.unpack('<I', hdr[0xd8:0xdc])[0]
        calc_hdr_crc = zlib.crc32(hdr[0x14 : 0x14 + 196])
        if hdr_crc != calc_hdr_crc:
            print(f"Error: header CRC mismatch (stored 0x{hdr_crc:08x}, calculated 0x{calc_hdr_crc:08x})", file=sys.stderr)
            return 1

        # Check uImage at offset 320
        uimage_magic_bytes = f.read(4)
        if len(uimage_magic_bytes) == 4:
            uimage_magic, = struct.unpack('>I', uimage_magic_bytes)
            if uimage_magic != IH_MAGIC:
                print(f"Warning: byte 320 is not standard uImage magic (0x{uimage_magic:08x})", file=sys.stderr)
            else:
                print(f"Verified: uImage magic 0x{uimage_magic:08x} present at offset +0x140 (320)")

        k_len = struct.unpack('<I', hdr[0x58:0x5c])[0]
        k_crc = struct.unpack('<I', hdr[0x60:0x64])[0]
        f.seek(HEADER_SIZE)
        k_bytes = f.read(k_len)
        if len(k_bytes) != k_len:
            print("Error: truncated kernel", file=sys.stderr)
            return 1
        calc_k_crc = zlib.crc32(k_bytes)
        if k_crc != calc_k_crc:
            print(f"Error: kernel CRC mismatch (stored 0x{k_crc:08x}, calculated 0x{calc_k_crc:08x})", file=sys.stderr)
            return 1

        r_off = struct.unpack('<I', hdr[0x78:0x7c])[0]
        r_len = struct.unpack('<I', hdr[0x64:0x68])[0]
        r_crc = struct.unpack('<I', hdr[0x6c:0x70])[0]

        file_r_off = r_off - FLASH_START_ADDR
        f.seek(file_r_off)
        r_bytes = f.read(r_len)
        if len(r_bytes) != r_len:
            print("Error: truncated rootfs", file=sys.stderr)
            return 1
        calc_r_crc = zlib.crc32(r_bytes)
        if r_crc != calc_r_crc:
            print(f"Error: rootfs CRC mismatch at flash 0x{r_off:06x} (stored 0x{r_crc:08x}, calculated 0x{calc_r_crc:08x})", file=sys.stderr)
            return 1

        k_alloc = struct.unpack('<I', hdr[0x74:0x78])[0]
        if HEADER_SIZE + k_len <= LOADER_SLOT < k_alloc:
            f.seek(LOADER_SLOT)
            kernel = f.read(k_alloc - LOADER_SLOT)
            try:
                kernel = kernel[:uimage_len(kernel, "Kernel")]
            except ValueError as e:
                print(f"Error: {e} at +0x{LOADER_SLOT:x}", file=sys.stderr)
                return 1
            dcrc, = struct.unpack('>I', kernel[24:28])
            if zlib.crc32(kernel[64:]) != dcrc:
                print(f"Error: kernel uImage data CRC mismatch at +0x{LOADER_SLOT:x}", file=sys.stderr)
                return 1
            print(f"Verified: kernel uImage at +0x{LOADER_SLOT:x} (flash 0x{FLASH_START_ADDR + LOADER_SLOT:06x})")
        # The kernel's mtdsplit parser takes the first SquashFS magic it finds.
        f.seek(0)
        head = f.read(file_r_off)
        for off in range(0, file_r_off, 4096):
            if head[off:off + 4] == b'hsqs':
                print(f"Error: stray SquashFS magic at +0x{off:x}, before the rootfs", file=sys.stderr)
                return 1

        tot_len = struct.unpack('<I', hdr[0x54:0x58])[0]
        print(f"Verified: 320-byte vendor header OK")
        print(f"  Header CRC: 0x{hdr_crc:08x} (VALID)")
        print(f"  Kernel size: {k_len} bytes, CRC: 0x{k_crc:08x} (VALID)")
        print(f"  RootFS flash offset: 0x{r_off:06x} (64k-aligned: {r_off % BLOCKSIZE == 0})")
        print(f"  RootFS size: {r_len} bytes, CRC: 0x{r_crc:08x} (VALID)")
        print(f"  RootFS magic: {r_bytes[:4]}")
        print(f"  Total header recorded size: {tot_len} bytes")
        print(f"  File actual size: {os.path.getsize(path)} bytes")
    return 0

def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--verify':
        sys.exit(verify_image(sys.argv[2]))

    loader = None
    if len(sys.argv) == 5 and sys.argv[1] == '--loader':
        with open(sys.argv[2], 'rb') as f:
            loader = f.read()
        del sys.argv[1:3]

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} [--loader <loader_uimage>] <input_kernel_rootfs> <output_wrapped_image>")
        print(f"       {sys.argv[0]} --verify <image_file>")
        sys.exit(2)

    in_path = sys.argv[1]
    out_path = sys.argv[2]

    with open(in_path, 'rb') as f:
        payload = f.read()

    wrapped = create_image(payload, loader)

    with open(out_path, 'wb') as f:
        f.write(wrapped)

    print(f"Successfully generated ZXIC vendor-wrapped image: {out_path} ({len(wrapped)} bytes)")

if __name__ == '__main__':
    main()
