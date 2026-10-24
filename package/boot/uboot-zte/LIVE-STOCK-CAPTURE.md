# Live stock capture — 2026-10-23

Capture directory (local build workspace, not publication material):
`/home/ali/openwrt-galachip/tmp/v2801sg-stock-live-20261023/`.
`files.json` contains sizes and SHA256 hashes; `commands.json`, `uart.log`,
`reset.log` and the export logs retain the command/result evidence.

The board initially ran the RAM UART-test monitor. It was responsive and was
software-reset into the installed March 2025 U-Boot; autoboot was interrupted.
This was a software reset, not an observed physical power cycle or a ROM
instruction trace. UART was released after returning the board to stock `=>`.

## Captured from stock before network initialization

`STOCK-SNAPSHOT.txt` collects version/help/environment, known clock/reset,
DDR/PHY, bus/pinmux, MAC and packet-engine registers, then the original first
4 KiB of SRAM. The peripheral reads and SRAM dump precede the first TFTP
operation of this session. FIFO/data windows and unknown addresses were not
read. The raw SRAM output was reconstructed as `stock-sram-prefix.bin`:

- Size 4096 bytes; byte-identical to the current NOR prefix.
- SHA256 `d2dcd2274745362995b4231746b4f6b4b7cc54b15f865294de913e24f11bc0f6`.
- Before TFTP, BMU control was zero and PP layout words at 92144018 began
  `50000000 50020000 52020000 00000800`. This is a distinct pre-network
  baseline, not the initialized layout captured after earlier TFTP tests.

## CPU context

The 160-byte source helper `tools/stock-context.S` was downloaded to 40500000
and returned successfully. It has no CP15 control writes, peripheral MMIO
accesses or stack use; it writes only the 72-byte result at 40530000. Build
and focused assembled-instruction checks passed. It reads 13 standard ARMv7
CPU registers, alongside caller CPSR/SP/LR and its own PC.

Artifact `v2801sg-stock-context-5cf4f113.bin`, SHA256
`5cf4f113b4c73773625da739982bb377181b8508cc4d902e5912bae29512a7b9`.
The CPU record is after stock TFTP and the `go` call, not ROM-entry state.

| Field | Value |
| --- | --- |
| CPSR | 600001d3 |
| Caller SP / LR | 41aabb68 / 41fbf608 |
| MIDR / MPIDR | 414fc091 / 80000000 |
| SCTLR / ACTLR | 08c5087a / 00000000 |
| TTBR0 / TTBR1 | 41ff0000 / 43000001 |
| TTBCR / DACR | 00000000 / ffffffff |
| VBAR | 00000000 |
| DFSR / IFSR | 000008a3 / 00001006 |
| DFAR / IFAR | 1c2154b0 / 300040d0 |

MMU is disabled. Table pointers are therefore not proof of valid active
translations or readable physical regions; TTBR1 must not be blindly
dereferenced. VBAR zero identifies the configured vector base, not a proven
BootROM mapping. Fault-status words may be stale. These values do not recover
the earlier ROM state that stock startup already overwrote.

## Fresh complete NOR export

The original 8 MiB backup predates the OpenWrt installation. A fresh read was
therefore taken after preserving the stock snapshot. A separate RAM monitor
variant adds the upstream `tftpput` command to the established chainload
configuration: no autoboot, empty boot command, environment in RAM, no
saveenv and automatic flash unlocking disabled. It uses the existing drivers.

Export image: `v2801sg-uboot-export-0ae31137.bin`, 234184 bytes, SHA256
`0ae311374e0720d26670684bfe92f3b6c6de751fa992b2373dac28f5390c3a63`.
Build log: `/tmp/v2801sg-snapshot-export-build.log`.

Only SPI reads were issued. Eight 1 MiB chunks were read through the bounded
40500000 buffer and uploaded using the existing TFTP service. Every received
chunk's CRC32 matched the board's CRC32 calculation before concatenation.
The temporary upload files were removed from the TFTP directory only after
verifying their saved copies. `nor-manifest.json` records the chunk hashes.

- `current-nor-8M.bin`: 8388608 bytes, SHA256
  `446cd10108602c87108d3b1db94e008e43666304d2b08e0879f3e74a588778e1`.
- `current-bootloader-256K.bin`: SHA256
  `857b170b3737fd9678eb68a8d4e8f0f50f298dccff9f39e286fbd2aedf8be34e`.
- The current first 192 KiB (00000..2ffff) is byte-identical to the original
  decompiled stock bootloader. Its SHA256 is
  `5d3dc762f04968dc31e5725e48a1bd27852c760156d9142176064c4fb5a5142f`.
- Seven bytes differ in the environment sector: offsets 30000, 30002, 30003,
  30246, 30247, 3024b, 3024c. The current versioninfo ends `0x9e 0x00`,
  already observed before this capture. No persistent environment operation
  was issued. Those bytes change the full boot-partition CRC32: the original
  backup is `072cb726`, the current partition `34d4cc5a`. Use SHA256/region
  comparison for exact-image identity.

The temporary UDP-1069 receiver got no successful upload; the monitor timed
out and returned normally. The existing TFTP service on port 69 then worked.
No firewall/service configuration was changed. After export, the RAM monitor
was software-reset and stock autoboot interrupted again.

## Coverage limits

This is a complete current NOR dump plus a scoped live stock snapshot. It is
not a complete dump of 32 MiB volatile DDR, every MMIO address or silicon ROM.
NOR and CPU evidence are retained separately because the RAM export monitor
changes runtime state. DDR contains active data and installed code; a later
dump would need its own capture method and cannot be called a pristine
power-on snapshot. Unknown MMIO/ROM windows remain excluded because reads
can have side effects or stall without a recoverable exception. No flash
write, saveenv or DDR reinitialization was performed.
