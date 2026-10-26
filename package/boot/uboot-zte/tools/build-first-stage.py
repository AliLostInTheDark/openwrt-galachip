#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Build the ZX279125 cold first stage around a monitor payload.

For a frozen candidate, pin the payload with --payload-sha256 so rebuilding the
package monitor never silently changes it. The U-Boot build tree is used only
for compiler flags and headers. The package build passes its own tree, payload
and cross prefix.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
TOOLS = ROOT / 'package/boot/uboot-zte/tools'
BOARD = ROOT / 'package/boot/uboot-zte/files/board/zte/zx279125'
PREFIX = str(ROOT / 'staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-muslgnueabi-')
UBOOT = ROOT / 'build_dir/target-arm_cortex-a9_musl_eabi/u-boot-vsol_v2801sg-uart-test/u-boot-v2026.10'
STOCK = ROOT / 'tmp/v2801sg-stock-live-20261023/current-bootloader-256K.bin'

STAGE_SLOT = 0x2000
BOOT_PARTITION = 0x40000


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def identity(data):
    return {'size': len(data), 'crc32': f'{zlib.crc32(data):08x}', 'sha256': sha256(data)}


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def uboot_cflags():
    """Reuse the exact command line U-Boot used for board/zte/zx279125/ddr.o."""
    line = (UBOOT / 'board/zte/zx279125/.ddr.o.cmd').read_text().splitlines()[0]
    args = shlex.split(line.split(':=', 1)[1].strip())
    args[0] = PREFIX + 'gcc'
    return [a for a in args if not a.startswith('-Wp,-MMD,')]


def compile_c(out, name, source, flags):
    args = uboot_cflags()
    args[-1] = str(source)
    args[args.index('-o') + 1] = str(out / f'{name}.o')
    args[1:1] = ['-I' + str(out), '-I' + str(BOARD), *flags]
    run(args, cwd=UBOOT)


def link(out, kind, objects):
    elf = out / f'{kind}.elf'
    run([PREFIX + 'ld', '-T', str(TOOLS / 'first-stage.lds'), *map(str, objects),
         '-Map=' + str(out / f'{kind}.map'), '-o', str(elf)])
    run([PREFIX + 'objcopy', '-O', 'binary', str(elf), str(out / f'{kind}.bin')])
    asm = run([PREFIX + 'objdump', '-d', str(elf)], capture_output=True, text=True).stdout
    (out / f'{kind}.asm').write_text(asm)
    return (out / f'{kind}.bin').read_bytes()


def main():
    global PREFIX, UBOOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload', type=Path, required=True)
    parser.add_argument('--payload-sha256')
    parser.add_argument('--uboot-tree', type=Path, default=UBOOT)
    parser.add_argument('--cross', default=PREFIX)
    parser.add_argument('--stock', type=Path, default=STOCK,
                        help='stock boot partition for the SPI audit; skipped if absent')
    parser.add_argument('--payload-map', type=Path,
                        help="the payload's System.map; records board_init_f for the emulator")
    parser.add_argument('--out', type=Path, required=True)
    opts = parser.parse_args()

    PREFIX, UBOOT = opts.cross, opts.uboot_tree.resolve()
    monitor = opts.payload.read_bytes()
    if opts.payload_sha256 and sha256(monitor) != opts.payload_sha256:
        parser.error(f'payload SHA256 {sha256(monitor)} != {opts.payload_sha256}')
    if STAGE_SLOT + ((len(monitor) + 255) & ~255) > BOOT_PARTITION:
        parser.error('payload does not fit the boot partition')

    out = opts.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    crc = zlib.crc32(monitor)
    (out / 'stage-payload.h').write_text(
        f'#define ZX_PAYLOAD_SIZE {len(monitor)}U\n#define ZX_PAYLOAD_CRC 0x{crc:08x}U\n')
    (out / 'monitor.bin').write_bytes(monitor)

    kinds = {'audit': ['-DZX_STAGE_RAM_AUDIT'], 'cold': []}
    manifest = {'payload': identity(monitor)}
    if opts.stock.exists():
        stock = opts.stock.read_bytes()
        assert len(stock) == BOOT_PARTITION
        spi_crc = zlib.crc32(stock[STAGE_SLOT:STAGE_SLOT + len(monitor)])
        manifest['spi_reference_crc32'] = f'{spi_crc:08x}'
        kinds['spi-audit'] = ['-DZX_STAGE_SPI_AUDIT']
        compile_c(out, 'spi-audit-body', TOOLS / 'first-stage.c',
                  ['-DZX_STAGE_SPI_AUDIT', f'-DZX_STAGE_SPI_CRC=0x{spi_crc:08x}U'])

    compile_c(out, 'ddr', BOARD / 'ddr.c', ['-DZX_DDR_TRACE', '-DZX_DDR_TRACE_BASE=0x00202420'])
    compile_c(out, 'spi', BOARD / 'early_spi.c', [])
    compile_c(out, 'cold-body', TOOLS / 'first-stage.c', [])
    compile_c(out, 'audit-body', TOOLS / 'first-stage.c', ['-DZX_STAGE_RAM_AUDIT'])
    run([PREFIX + 'gcc', '-march=armv7-a', '-marm', '-DZX_EARLY_TRACE=0x00202500',
         '-DZX_EARLY_FAULT_STACK=0x00203000', '-DZX_EARLY_FAULT_UART', '-c', str(TOOLS / 'early-cpu.S'),
         '-o', str(out / 'cpu.o')])

    if opts.payload_map:
        symbols = dict(reversed(line.split()[::2]) for line in
                       opts.payload_map.read_text().splitlines())
        manifest['payload']['board_init_f'] = '0x' + symbols['board_init_f']
    for kind, flags in kinds.items():
        entry = out / f'{kind}-entry.o'
        run([PREFIX + 'gcc', *flags, '-march=armv7-a', '-marm', '-c',
             str(TOOLS / 'first-stage.S'), '-o', str(entry)])
        objects = [entry, out / f'{kind}-body.o']
        if kind == 'cold':
            objects += [out / 'cpu.o', out / 'ddr.o', out / 'spi.o']
        elif kind == 'spi-audit':
            objects += [out / 'spi.o']
        manifest[kind] = identity(link(out, kind, objects))

    stage = (out / 'cold.bin').read_bytes()
    combined = (stage + b'\xff' * (STAGE_SLOT - len(stage)) + monitor +
                b'\xff' * (-len(monitor) % 256))
    assert len(combined) <= BOOT_PARTITION
    (out / 'combined-OFFLINE-NOT-FOR-FLASH.bin').write_bytes(combined)
    manifest['combined'] = identity(combined)

    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
