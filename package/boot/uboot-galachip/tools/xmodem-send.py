#!/usr/bin/env python3
"""Paced XMODEM-CRC sender for the stock G7 U-Boot `lx` command.

U-Boot's xm.c sleeps 1 ms whenever no byte is waiting, which overruns the
UART FIFO at full 115200 line rate. This sender keeps 128-byte XMODEM-CRC
blocks but writes them in small chunks with short gaps.

Usage: xmodem-send.py [--chunk N] [--gap SECONDS] DEVICE FILE
Run with the terminal program closed (the port must not be shared).
"""
import argparse
import os
import select
import sys
import termios
import time

SOH, EOT, ACK, NAK, CAN, CRC_REQ = 0x01, 0x04, 0x06, 0x15, 0x18, ord("C")
BLOCK = 128


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                   # iflag: raw
    attrs[1] = 0                                   # oflag: raw
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                   # lflag: raw
    attrs[4] = attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def read_byte(fd, timeout):
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return None
    data = os.read(fd, 1)
    return data[0] if data else None


def paced_write(fd, data, chunk, gap, is_tty):
    for i in range(0, len(data), chunk):
        os.write(fd, data[i:i + chunk])
        if is_tty:
            termios.tcdrain(fd)
        if gap:
            time.sleep(gap)


def wait_reply(fd, timeout):
    """Return ACK, NAK or CAN; ignore stray CRC requests."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        byte = read_byte(fd, deadline - time.monotonic())
        if byte in (ACK, NAK, CAN):
            return byte
    return None


def send(fd, payload, chunk, gap, is_tty=True, retries=10):
    print("Waiting for receiver 'C' (type lx in U-Boot first)...", flush=True)
    while True:
        byte = read_byte(fd, 60)
        if byte is None:
            sys.exit("No CRC request within 60 s")
        if byte == CRC_REQ:
            break

    blocks = (len(payload) + BLOCK - 1) // BLOCK
    naks = 0
    start = time.monotonic()
    for n in range(blocks):
        data = payload[n * BLOCK:(n + 1) * BLOCK].ljust(BLOCK, b"\x1a")
        seq = (n + 1) & 0xFF
        crc = crc16(data)
        packet = bytes([SOH, seq, 0xFF - seq]) + data + bytes([crc >> 8, crc & 0xFF])
        for attempt in range(retries + 1):
            paced_write(fd, packet, chunk, gap, is_tty)
            reply = wait_reply(fd, 5)
            if reply == ACK:
                break
            if reply == CAN:
                sys.exit(f"Receiver cancelled at block {n + 1}")
            naks += 1
        else:
            sys.exit(f"Block {n + 1} failed after {retries} retries")
        if (n + 1) % 1024 == 0 or n + 1 == blocks:
            rate = (n + 1) * BLOCK / (time.monotonic() - start)
            print(f"{n + 1}/{blocks} blocks, {naks} retries, {rate / 1024:.1f} KiB/s",
                  flush=True)

    for _ in range(retries):
        os.write(fd, bytes([EOT]))
        if wait_reply(fd, 5) == ACK:
            print(f"Done: {blocks * BLOCK} bytes in {time.monotonic() - start:.0f} s, "
                  f"{naks} retries")
            return
    sys.exit("No ACK for EOT")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--chunk", type=int, default=8, help="bytes per write (default 8)")
    parser.add_argument("--gap", type=float, default=0.0012,
                        help="seconds between writes (default 0.0012)")
    parser.add_argument("device")
    parser.add_argument("file")
    args = parser.parse_args()

    with open(args.file, "rb") as f:
        payload = f.read()
    fd = open_port(args.device)
    try:
        send(fd, payload, args.chunk, args.gap)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
