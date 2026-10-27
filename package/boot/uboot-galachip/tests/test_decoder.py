#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production decoder with the pinned upstream LZMA SDK on the host."""
import ctypes
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_images import images, ROOT

class Decoder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(os.environ["UBOOT_SRC"])
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name) / "decoder.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-fsanitize=undefined",
                        "-DZ7_LZMA_SIZE_OPT", "-I" + str(ROOT / "stages"),
                        "-I" + str(ROOT / "stages/compat"),
                        "-I" + str(source / "lib/lzma"),
                        str(ROOT / "stages/common.c"), str(ROOT / "stages/decode.c"),
                        str(ROOT / "tests/decoder_mock.c"),
                        str(source / "lib/lzma/LzmaDec.c"), "-o", str(lib)], check=True)
        cls.c = ctypes.CDLL(str(lib))
        cls.c.test_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def decode(self, raw, packed=None, fields=None, capacity=None):
        valid, header = images.compress(raw)
        packed = valid if packed is None else packed
        values = list(struct.unpack("<8I", header))
        values[1], values[3] = len(packed), images.crc(packed)
        if fields:
            for key, value in fields.items():
                values[key] = value
        header = images.words_with_crc(values[:7])
        capacity = len(raw) if capacity is None else capacity
        out = ctypes.create_string_buffer(b"!" * (len(raw) + 32))
        ret = self.c.test_decode(header, packed, len(packed), out, capacity)
        self.assertEqual(out.raw[len(raw):len(raw) + 32], b"!" * 32)
        return ret, out.raw[:len(raw)]

    def test_actual_sdk_roundtrip(self):
        for raw in (b"x", bytes(range(256)) * 1024):
            self.assertEqual(self.decode(raw), (0, raw))

    def test_built_production_payload(self):
        build = Path(os.environ["UBOOT_SRC"]).parent
        raw = (build / "production/u-boot.bin").read_bytes()
        packed = (build / "stages/proper.lzma").read_bytes()
        header = (build / "stages/meta.bin").read_bytes()
        out = ctypes.create_string_buffer(len(raw))
        ret = self.c.test_decode(header, packed, len(packed), out, len(raw))
        self.assertEqual((ret, out.raw), (0, raw))

    def test_truncation_recomputed_crc(self):
        raw = bytes(range(256)) * 128
        packed, _ = images.compress(raw)
        for n in (0, 13, 17, len(packed) // 2, len(packed) - 1):
            self.assertNotEqual(self.decode(raw, packed[:n])[0], 0)

    def test_corrupt_stream_recomputed_crc(self):
        raw = bytes(range(256)) * 128
        packed, _ = images.compress(raw)
        for offset in (13, 16, len(packed) // 2, len(packed) - 5):
            bad = bytearray(packed)
            bad[offset] ^= 255
            self.assertNotEqual(self.decode(raw, bytes(bad))[0], 0)

    def test_profile_and_declared_size(self):
        raw = b"test image" * 100
        packed, _ = images.compress(raw)
        for offset, value in ((0, 255), (4, 255), (5, 0), (12, 1)):
            bad = bytearray(packed)
            bad[offset] = value
            self.assertNotEqual(self.decode(raw, bytes(bad))[0], 0)

    def test_output_capacity_checksum_entry_and_trailing_bytes(self):
        raw = b"x" * 1024
        self.assertNotEqual(self.decode(raw, capacity=1023)[0], 0)
        for fields in ({2: 0xffffffff}, {4: 0}, {5: 0x40800000}, {6: 0x40100000}):
            self.assertNotEqual(self.decode(raw, fields=fields)[0], 0)
        packed, _ = images.compress(raw)
        self.assertNotEqual(self.decode(raw, packed + b"x")[0], 0)

if __name__ == "__main__":
    unittest.main(verbosity=2)
