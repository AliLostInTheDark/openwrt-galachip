#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the image tools: ROM header, legacy images, boot region, flash image."""
import importlib.util
import lzma
import os
import random
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("images", ROOT / "tools/images.py")
images = importlib.util.module_from_spec(spec)
spec.loader.exec_module(images)
sys.path.insert(0, str(ROOT / "tools"))
import flash_image

def legacy(data, load, comp=0, arch=2, kind=5):
    """A mkimage legacy image, built here so the tests need no U-Boot tree."""
    header = struct.pack(">7I4B32s", 0x27051956, 0, 0, len(data), load, load,
                         images.crc(data), 17, arch, kind, comp, b"test")
    return header[:4] + struct.pack(">I", images.crc(header)) + header[8:] + data

class Images(unittest.TestCase):
    def valid(self, image, limit=0x8000):
        try:
            return images.legacy_image(image, 0x40100000, "none", limit)
        except ValueError:
            return 0

    def test_rom_checksum_and_limit(self):
        for n in (1, 4, 0x1bff, 0x1c00):
            raw = bytes(random.Random(n).randrange(256) for _ in range(n))
            wrapped = images.rom_image(raw)
            magic, count, reserved, checksum = struct.unpack("<4I", wrapped[:16])
            self.assertEqual((magic, reserved), (0x03020100, 0))
            self.assertEqual(count, len(wrapped) - 16)
            self.assertEqual(checksum, sum(struct.unpack(
                "<" + "I" * (count // 4), wrapped[16:])) & 0xffffffff)
            self.assertEqual(images.rom_valid(wrapped), len(wrapped))
        for raw in (b"", b"x" * 0x1c01):
            with self.assertRaises(ValueError):
                images.rom_image(raw)
        bad = bytearray(images.rom_image(b"abcd"))
        bad[16] ^= 1
        with self.assertRaises(ValueError):
            images.rom_valid(bytes(bad))

    def test_legacy_image_valid(self):
        for n in (1, 4, 0x7fc0):
            image = legacy(bytes(random.Random(n).randrange(256) for _ in range(n)),
                           0x40100000)
            self.assertEqual(self.valid(image), len(image))
            self.assertEqual(self.valid(image + b"\xff" * 100), len(image))
            self.assertFalse(self.valid(image[:-1]))

    def test_legacy_image_every_header_byte(self):
        image = legacy(b"x" * 100, 0x40100000)
        for i in range(64):
            bad = bytearray(image)
            bad[i] ^= 1
            self.assertFalse(self.valid(bytes(bad)), i)
        bad = bytearray(image)
        bad[100] ^= 1
        self.assertFalse(self.valid(bytes(bad)))

    def test_legacy_image_fields_with_valid_crcs(self):
        for image in (legacy(b"x" * 4, 0x40010000), legacy(b"x" * 4, 0x40100000, comp=3),
                      legacy(b"x" * 4, 0x40100000, arch=5), legacy(b"x" * 4, 0x40100000, kind=2),
                      legacy(b"", 0x40100000), legacy(b"x" * 0x7fc1, 0x40100000)):
            self.assertFalse(self.valid(image))
        for n in (0, 63):
            self.assertFalse(self.valid(bytes(n)))

    def test_lzma_and_region(self):
        raw = bytes(range(256)) * 512
        packed = images.lzma_alone(raw)
        self.assertEqual(lzma.decompress(packed), raw)
        proper = legacy(packed, 0x40010000, comp=3)
        spl = legacy(b"\xe5" * 0x7fc0, 0x40100000)
        region = images.boot_region(images.rom_image(b"x" * 8), spl, proper)
        self.assertEqual(len(region), 0x20000)
        self.assertEqual(region[0x2000:0x2000 + len(spl)], spl)
        self.assertEqual(region[0xa000:0xa000 + len(proper)], proper)
        for bad in (legacy(b"\xe5" * 0x7fc1, 0x40100000), legacy(packed, 0x40100000, comp=3)):
            with self.assertRaises(ValueError):
                images.boot_region(images.rom_image(b"x" * 8), bad, proper)
        with self.assertRaises(ValueError):
            images.boot_region(images.rom_image(b"x" * 8), spl, legacy(packed, 0x40010000))
        with self.assertRaises(ValueError):
            images.boot_region(images.rom_image(b"x" * 8), spl,
                               legacy(bytes(0x16000 - 63), 0x40010000, comp=3))
        for raw in (b"", bytes(0xe0001)):
            with self.assertRaises(ValueError):
                images.lzma_alone(raw)

class FlashImage(unittest.TestCase):
    def backup(self):
        data = bytearray(os.urandom(flash_image.CHIP_SIZE))
        data[0x20000:0x20004] = struct.pack("<I", flash_image.FIRMWARE_MAGIC)
        return bytes(data)

    def region(self):
        return images.boot_region(images.rom_image(b"x" * 8),
                                  legacy(b"\xe5" * 64, 0x40100000),
                                  legacy(images.lzma_alone(b"u" * 4096), 0x40010000, comp=3))

    def test_only_boot_region_changes(self):
        backup, region = self.backup(), self.region()
        image = flash_image.build(backup, region)
        self.assertEqual(image[:0x20000], region)
        self.assertEqual(image[0x20000:], backup[0x20000:])

    def test_rejects_bad_inputs(self):
        backup, region = self.backup(), self.region()
        for bad in (region[:-1], region[:0x2050] + b"\0" + region[0x2051:],
                    region[:0xa000] + b"\0" + region[0xa001:], b"\1" + region[1:]):
            with self.assertRaises(ValueError):
                flash_image.build(backup, bad)
        for bad in (backup[:-1], backup[:0x20000] + b"\0" * 4 + backup[0x20004:]):
            with self.assertRaises(ValueError):
                flash_image.build(bad, region)

if __name__ == "__main__":
    unittest.main(verbosity=2)
