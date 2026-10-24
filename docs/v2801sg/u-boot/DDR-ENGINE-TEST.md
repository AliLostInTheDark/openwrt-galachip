# Combined SRAM engine-reset and DDR recovery test

Prepared 2026-10-23; hardware execution failed as recorded below. Artifact
92e55c1c is retired: do not rerun it. This was a supervised RAM test
that invalidates the current DRAM session. It cannot return to that session.
Success reloads the installed stock bootloader from SPI and jumps to it;
failure halts in SRAM and requires a power-cycle. Keep fiber disconnected,
stay at the desk, and stop immediately if unresponsive. No flash writes.

## Prerequisites established separately

The user passed the UART drain/return audit, reversible DMA/BMU disable and
restore, PP reset with DMA/BMU kept disabled, and physical power-cycle recovery
to stock U-Boot with a successful ping. These support trying the combined
sequence; they do not prove global AXI inactivity, working DDR replay, or the
mask-ROM loading contract. The older failed DDR and drain-only artifacts remain
retired. This is not an image for installation or an independent cold boot.

## Artifact and verification

`v2801sg-sram-ddr-engine-92e55c1c.bin`: 3305 bytes (`ce9` hex), CRC32
`424ebc83`, SHA256
`92e55c1c38c03d0e98bec558117e63eff521248059621e6ede049396b7070a3e`.
Staged in `/srv/tftp/`. Sources: `tools/sram-ddr-recovery.S` compiled with
`ZX_DDR_STOP_ENGINE`, `tools/sram-engine-probe.c` with `ZX_ENGINE_RESET`, and
the current board `ddr.o`, `early_spi.o`, `early_boot.o`.

The linker bound is tightened to 00200e00, leaving the engine result record
clear of code/rodata. Empty data/BSS and no undefined symbols were checked.
Matching short-wchar ABI rebuild is byte-identical. Disassembly, manifest,
map, ELF and entry-model checks are in `/tmp/v2801sg-ddr-engine-recovery/`.

The entry masks interrupts, uses the previously tested SRAM stack, rejects
enabled caches/MMU and active MAC RX, and calls the tested engine-reset code.
It refuses any engine error, enabled DMA mask, pending PP request, nonzero BMU,
or enabled MAC RX/TX before DDR. It prints and drains its banner, invokes the
source DDR/SPI loader for flash 0..3ffff -> DRAM 41f00000, checks stock header
words, drains the handoff message, and jumps to 41f00000.

The assembled-entry model checks call order and prevents DDR entry on engine
failure, bad control readbacks or UART drain timeout. DDR write/delay tracing
still matches all 38 vendor events; the dispatcher model checks copy bounds
and error propagation. The engine model covers pre-reset rollback and keeping
controls disabled after reset. Models stub hardware behavior; the combined
operation remains unverified electrically.

## From the stock prompt after the recovery power-cycle

The stock monitor lacks crc32. Require the exact monitor size before `go`:

```text
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.1
tftp 0x40400000 v2801sg-uboot-stop-7f1be111.bin
go 0x40400000
```

Require 232480 bytes (`38c20`) and the new `ZX279125 #` prompt. Then:

```text
setenv ethaddr 70:b6:4f:01:a6:7a
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.100
tftp 0x40500000 v2801sg-sram-ddr-engine-92e55c1c.bin
crc32 0x40500000 0xce9
cp.b 0x40500000 0x00200000 0xce9
crc32 0x00200000 0xce9
dcache off
icache off
```

Require 3305 bytes and CRC 424ebc83 in both locations. Only then:

```text
go 0x00200000
```

Capture all output. Expect the complete SRAM DDR-recovery banner, then
`DDR/SPI/image header OK; entering stock U-Boot.` and a stock startup banner.
Interrupt stock autoboot. Do not run upgrades, saveenv, another DDR test or
restore SRAM from the old DRAM backup: DRAM contents are no longer retained
by contract. If output stops or repeats, stop and recover by a normal physical
power-cycle with button released. Successful warm replay would still require
separate cold-boot and ROM-loader work before any bootloader replacement.

## Failed hardware result — 2026-10-23

User capture: `/home/ali/Desktop/uboot-test.txt`. Both copies matched CRC
424ebc83 and transfer size 3305. After the final `go`, the entire SRAM banner
printed, followed by sustained line-feed output. No DDR/SPI handoff message,
stock startup banner or normal return was observed. This is a failure, not
working DDR initialization. The engine and control guards were passed before
the complete banner, based on the assembled entry order. The banner alone
does not prove the following UART drain returned or locate the first failing
DDR/PLL access. Static control flow has no intentional newline-print loop;
UART clock/reset behavior and CPU execution failure remain possible causes.

The earlier queued-banner hypothesis is insufficient to explain this result:
the full banner is now visible and the tested engine-reset sequence was
included. Keep both changes as independently tested improvements, but do not
describe either as a proven fix for the DDR failure. Further hardware tests
are paused until physical recovery; do not issue speculative UART, MMIO or
DDR commands to the failed session. Recover through a normal power-cycle,
button released, interrupt stock autoboot and confirm its prompt. Subsequent
work must localize the clock/DDR failure before another combined attempt.
