#!/usr/bin/env python3
"""Turn U-Boot `md.l` output captured from the serial console into a binary.

    md2bin.py rom.log rom.bin

Lines look like `00000000: ea00001b ea000005 ea000005 ea000005    ....`.
Words are little-endian. Gaps and conflicting repeats are reported.
"""
import re
import struct
import sys

LINE = re.compile(r"^([0-9a-f]{8}):((?: [0-9a-f]{8}){1,4})", re.I)


def main(src, dst):
    words = {}
    for raw in open(src, errors="replace"):
        m = LINE.match(raw.strip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            a = addr + 4 * i
            v = int(w, 16)
            if a in words and words[a] != v:
                print(f"conflict at {a:08x}: {words[a]:08x} vs {v:08x}")
            words[a] = v
    if not words:
        sys.exit("no md.l lines found")

    start, end = min(words), max(words) + 4
    missing = [a for a in range(start, end, 4) if a not in words]
    if missing:
        print(f"{len(missing)} missing words, first at {missing[0]:08x}")
    data = b"".join(struct.pack("<I", words.get(a, 0)) for a in range(start, end, 4))
    open(dst, "wb").write(data)
    print(f"{dst}: {start:08x}-{end:08x}, {len(data)} bytes")

    # Where the image stops: the last 1 KiB block that is not all 0xff/0x00.
    last = 0
    for off in range(0, len(data), 1024):
        block = data[off:off + 1024]
        if block.strip(b"\xff") and block.strip(b"\x00"):
            last = off + 1024
    print(f"content ends near {start + last:08x}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
