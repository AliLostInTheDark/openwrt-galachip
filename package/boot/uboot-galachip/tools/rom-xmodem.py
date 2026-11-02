#!/usr/bin/env python3
"""Upload an SPL to the GC1601 mask ROM over XMODEM-CRC.

The ROM repeats 'C' on every read timeout until the first block arrives and
cancels on a repeated block number, so a stale 'C' must never count as NAK:
only ACK advances, NAK resends, CAN aborts. The ROM runs what it receives
from its first byte, so a 16-byte ROM header is stripped first.

Standalone: selects menu option 2, sends, then stops U-Boot's autoboot.
    rom-xmodem.py /dev/ttyUSB1 image.bin

As picocom's send command (choose 2 + Enter yourself, then C-a C-s):
    picocom -b 115200 --send-cmd "rom-xmodem.py" /dev/ttyUSB1
"""
import os
import select
import struct
import sys
import termios
import time

SOH, EOT, ACK, NAK, CAN = 0x01, 0x04, 0x06, 0x15, 0x18
ROM_MAGIC = 0x03020100


class StdioPort:
    """The serial port as picocom hands it over: stdin and stdout."""

    def read(self, size=1):
        if not select.select([0], [], [], 0.1)[0]:
            return b''
        return os.read(0, size)

    def write(self, data):
        os.write(1, data)

    def reset_input_buffer(self):
        termios.tcflush(0, termios.TCIFLUSH)


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1
    return crc & 0xffff


def wait_for(port, wanted, timeout):
    end = time.time() + timeout
    while time.time() < end:
        c = port.read(1)
        if c and c[0] in wanted:
            return c[0]
    return None


def payload(path):
    data = open(path, 'rb').read()
    if len(data) >= 16 and struct.unpack('<I', data[:4])[0] == ROM_MAGIC:
        size = struct.unpack('<I', data[4:8])[0]
        data = data[16:16 + size]
    return data


def send(port, data, log):
    if wait_for(port, b'C', 30) is None:
        sys.exit('no C from the ROM: choose 2 and Enter at its menu first')
    time.sleep(0.2)

    blocks = [data[i:i + 128].ljust(128, b'\x1a') for i in range(0, len(data), 128)]
    for n, block in enumerate(blocks, 1):
        frame = bytes([SOH, n & 0xff, 0xff - (n & 0xff)]) + block + crc16(block).to_bytes(2, 'big')
        for attempt in range(10):
            port.reset_input_buffer()
            port.write(frame)
            reply = wait_for(port, (ACK, NAK, CAN), 3)
            if reply == ACK:
                break
            if reply == CAN:
                sys.exit(f'ROM cancelled at block {n}')
        else:
            sys.exit(f'no ACK for block {n}')
        log.write(f'\rblock {n}/{len(blocks)}')
        log.flush()

    port.write(bytes([EOT]))
    log.write('\nEOT %s\n' % ('acked' if wait_for(port, (ACK,), 3) == ACK else 'not acked'))


def standalone(dev, data):
    import serial

    port = serial.Serial(dev, 115200, timeout=0.1)
    port.reset_input_buffer()
    port.write(b'2\r')
    send(port, data, sys.stdout)

    # Stop U-Boot's 1 s autoboot so the SPL can be rewritten from its prompt.
    end = time.time() + 15
    seen = b''
    while time.time() < end:
        out = port.read(256)
        if out:
            sys.stdout.write(out.decode('latin-1'))
            sys.stdout.flush()
            seen += out
            if b'Hit any key' in seen:
                port.write(b' ')
                seen = b''


def main():
    if len(sys.argv) == 3:
        standalone(sys.argv[1], payload(sys.argv[2]))
    elif len(sys.argv) == 2:
        send(StdioPort(), payload(sys.argv[1]), sys.stderr)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
