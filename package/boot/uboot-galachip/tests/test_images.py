#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual freestanding C validators and XMODEM receiver."""
import ctypes
import importlib.util
import lzma
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("images", ROOT / "tools/images.py")
images = importlib.util.module_from_spec(spec)
spec.loader.exec_module(images)
sys.path.insert(0, str(ROOT / "tools"))
import flash_image

def packet(seq, data, corrupt=False):
    crc = 0
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 65535
    if corrupt:
        crc ^= 1
    return [1 if len(data) == 128 else 2, seq & 255, (~seq) & 255,
            *data, crc >> 8, crc & 255]

class Images(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name) / "stages.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", str(lib),
                        str(ROOT / "stages/common.c"), str(ROOT / "stages/xmodem.c"),
                        str(ROOT / "tests/uart_mock.c")], check=True)
        cls.c = ctypes.CDLL(str(lib))
        cls.c.stage_valid.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.c.lzma_header_valid.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.c.uart_image_valid.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.c.xmodem_receive.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.c.uart_script.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.c.uart_output_count.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def valid(self, data):
        buf = ctypes.create_string_buffer(data)
        return self.c.stage_valid(buf, len(data))

    def receive(self, sequence, capacity):
        incoming = (ctypes.c_int * len(sequence))(*sequence)
        out = ctypes.create_string_buffer(b"!" * (capacity + 16))
        self.c.uart_script(incoming, len(sequence))
        result = self.c.xmodem_receive(out, capacity)
        self.assertEqual(out.raw[capacity:capacity + 16], b"!" * 16)
        return result, out.raw[:capacity]

    def test_rom_checksum_and_limit(self):
        for n in (1, 4, 0x1bff, 0x1c00):
            raw = bytes(random.Random(n).randrange(256) for _ in range(n))
            wrapped = images.rom_image(raw)
            magic, count, reserved, checksum = struct.unpack("<4I", wrapped[:16])
            self.assertEqual((magic, reserved), (0x03020100, 0))
            self.assertEqual(count, len(wrapped) - 16)
            self.assertEqual(checksum, sum(struct.unpack(
                "<" + "I" * (count // 4), wrapped[16:])) & 0xffffffff)
        for raw in (b"", b"x" * 0x1c01):
            with self.assertRaises(ValueError):
                images.rom_image(raw)

    def test_wrapper_limit_and_bootregion(self):
        for n in (64, 0x1dfe0):
            image = images.proper_image(bytes(n))
            self.assertTrue(self.valid(image))
            region = images.boot_region(images.rom_image(b"x"), image)
            self.assertEqual(len(region), 0x20000)
            self.assertEqual(region[0x2000:], image)
        with self.assertRaises(ValueError):
            images.proper_image(bytes(0x1dfe1))

    def test_footer_all_fields_and_truncation(self):
        good = images.proper_image(b"x" * 100)
        for i in range(32):
            bad = bytearray(good)
            bad[-32 + i] ^= 1
            self.assertFalse(self.valid(bytes(bad)))
        for n in (0, 1, 31, 64, 0x1dfff):
            self.assertFalse(self.valid(good[:n]))
        bad = bytearray(good)
        bad[50] ^= 1
        self.assertFalse(self.valid(bytes(bad)))

    def test_footer_overflow_with_valid_crc(self):
        for size in (0, 63, 0x1dfe1, 0xffffffff):
            image = images.proper_image(bytes(128))
            footer = images.words_with_crc(
                [0x31503747, 1, size, 0x40100000, 0, 0, 0])
            self.assertFalse(self.valid(image[:-32] + footer))

    def test_lzma_roundtrip_and_lengths(self):
        for raw in (b"x", bytes(range(256)) * 512):
            packed, header = images.compress(raw)
            self.assertEqual(lzma.decompress(packed), raw)
            self.assertTrue(self.c.lzma_header_valid(header, len(packed)))
            for i in range(32):
                bad = bytearray(header)
                bad[i] ^= 1
                self.assertFalse(self.c.lzma_header_valid(bytes(bad), len(packed)))
            self.assertFalse(self.c.lzma_header_valid(header, len(packed) - 1))
        for raw in (b"", bytes(0xe0001)):
            with self.assertRaises(ValueError):
                images.compress(raw)

    def test_xmodem_128_and_1024(self):
        for n in (128, 1024):
            data = bytes(i & 255 for i in range(n))
            result, out = self.receive(packet(1, data) + [4], n)
            self.assertEqual(result, n)
            self.assertEqual(out, data)

    def test_initial_timeout_requests_crc_without_nak(self):
        data = b"a" * 128
        result, out = self.receive([-1, -1] + packet(1, data) + [4], 128)
        self.assertEqual((result, out), (128, data))
        self.assertEqual(self.c.uart_output_count(67), 3)
        self.assertEqual(self.c.uart_output_count(21), 0)

    def test_initial_junk_is_drained_once(self):
        data = b"a" * 128
        result, out = self.receive([0x55] * 70 + [-1] + packet(1, data) + [4], 128)
        self.assertEqual((result, out), (128, data))
        self.assertEqual(self.c.uart_output_count(67), 2)
        self.assertEqual(self.c.uart_output_count(21), 0)

    def test_bad_first_packet_requests_crc_without_nak(self):
        data = b"a" * 128
        result, out = self.receive(packet(1, data, True) + [-1] +
                                   packet(1, data) + [4], 128)
        self.assertEqual((result, out), (128, data))
        self.assertEqual(self.c.uart_output_count(67), 2)
        self.assertEqual(self.c.uart_output_count(21), 0)

    def test_lost_header_gets_one_nak(self):
        first, second = b"a" * 128, b"b" * 128
        stream = (packet(1, first) + packet(2, second)[1:] + [-1, -1] +
                  packet(2, second) + [4])
        result, out = self.receive(stream, 256)
        self.assertEqual((result, out), (256, first + second))
        self.assertEqual(self.c.uart_output_count(21), 1)

    def test_continuous_noise_cancels(self):
        result, _ = self.receive([0x55] * 200000, 128)
        self.assertEqual(result, -1)
        self.assertEqual(self.c.uart_output_count(24), 2)

    def test_uart_package_and_overflow(self):
        for n in (1, 128, images.PROPER_MAX):
            packed = images.uart_image(bytes(n))
            self.assertTrue(self.c.uart_image_valid(packed, len(packed)))
            for offset in (0, 4, 8, 12, 16):
                bad = bytearray(packed)
                bad[offset] ^= 255
                self.assertFalse(self.c.uart_image_valid(bytes(bad), len(bad)))
        packed = struct.pack("<4I", 0x31523747, 0xffffffff, 0x41000000, 0)
        self.assertFalse(self.c.uart_image_valid(packed, len(packed)))

    def test_crc_failure_never_copies(self):
        data = b"a" * 128
        result, out = self.receive(packet(1, data, True) + [-1, 24], 128)
        self.assertEqual(result, -1)
        self.assertEqual(out, b"!" * 128)
        self.assertEqual(self.c.uart_output_count(21), 0)

    def test_crc_retry_and_duplicate(self):
        data = b"a" * 128
        result, out = self.receive(packet(1, data, True) + [-1] +
                                   packet(1, data) + packet(1, data) + [4], 128)
        self.assertEqual((result, out), (128, data))

    def test_duplicate_must_match(self):
        for duplicate in (b"b" * 128, b"a" * 1024):
            result, _ = self.receive(packet(1, b"a" * 128) +
                                      packet(1, duplicate) + [4], 1024)
            self.assertEqual(result, -1)

    def test_duplicate_budget(self):
        stream = packet(1, b"a" * 128) * 66 + [4]
        result, _ = self.receive(stream, 128)
        self.assertEqual(result, -1)

    def test_sequence_wrap(self):
        stream, wanted = [], b""
        for seq in range(1, 259):
            data = bytes([seq & 255]) * 128
            stream += packet(seq, data)
            wanted += data
        result, out = self.receive(stream + [4], len(wanted))
        self.assertEqual((result, out), (len(wanted), wanted))

    def test_truncated_packet_timeout_cancel(self):
        for stream in ([], [1], [1, 1, 254, 0], [24], [4],
                       packet(2, b"a" * 128) + [4]):
            result, out = self.receive(stream, 128)
            self.assertEqual(result, -1)
            self.assertEqual(out, b"!" * 128)

    def test_capacity_and_corrupt_sequence(self):
        for capacity in (0, 127):
            result, _ = self.receive(packet(1, b"a" * 128) + [4], capacity)
            self.assertEqual(result, -1)
        stream = packet(1, b"a" * 128)
        stream[2] = 0
        result, out = self.receive(stream + [-1, 24], 128)
        self.assertEqual(result, -1)
        self.assertEqual(out, b"!" * 128)

class FlashImage(unittest.TestCase):
    def backup(self):
        data = bytearray(os.urandom(flash_image.CHIP_SIZE))
        data[0x20000:0x20004] = struct.pack("<I", flash_image.FIRMWARE_MAGIC)
        return bytes(data)

    def test_only_boot_region_changes(self):
        backup = self.backup()
        proper = images.proper_image(b"\xe5" * 4096)
        image = flash_image.build(backup, proper)
        self.assertEqual(image[:0x2000], backup[:0x2000])
        self.assertEqual(image[0x2000:0x20000], proper)
        self.assertEqual(image[0x20000:], backup[0x20000:])
        spl = images.rom_image(b"\x01" * 64)
        image = flash_image.build(backup, proper, spl)
        self.assertEqual(image[:len(spl)], spl)
        self.assertEqual(image[0x20000:], backup[0x20000:])

    def test_rejects_bad_inputs(self):
        backup = self.backup()
        proper = images.proper_image(b"\xe5" * 4096)
        for bad in (proper[:-1], proper[:100] + b"\0" + proper[101:],
                    proper[:-4] + b"\0\0\0\0"):
            with self.assertRaises(ValueError):
                flash_image.build(backup, bad)
        for bad in (backup[:-1], backup[:0x20000] + b"\0" * 4 + backup[0x20004:]):
            with self.assertRaises(ValueError):
                flash_image.build(bad, proper)
        spl = bytearray(images.rom_image(b"\x01" * 64))
        spl[20] ^= 1
        with self.assertRaises(ValueError):
            flash_image.build(backup, proper, bytes(spl))

if __name__ == "__main__":
    unittest.main(verbosity=2)
