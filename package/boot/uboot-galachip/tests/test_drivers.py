#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
headers = ["dm.h", "net.h", "phy.h", "miiphy.h", "clk.h", "clk-uclass.h", "spi.h", "spi-mem.h",
           "asm/io.h", "asm/barriers.h", "linux/delay.h", "linux/iopoll.h", "linux/err.h",
           "linux/types.h", "linux/bitfield.h", "dm/device_compat.h"]
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name in headers:
        header = tmp / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "mock.h"\n')
    arch = tmp / "asm/arch/gc1601.h"
    arch.parent.mkdir(parents=True, exist_ok=True)
    arch.write_text('#include "%s"\n' % (ROOT / "files/arch/arm/include/asm/arch-galachip/gc1601.h"))
    exe = tmp / "drivers"
    subprocess.run(["cc", "-std=gnu11", "-fsanitize=undefined", "-g",
                    "-Wno-int-to-pointer-cast", "-I" + str(tmp),
                    "-I" + str(ROOT / "tests"), "-I" + str(ROOT / "files/include"), str(ROOT / "tests/driver_test.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
