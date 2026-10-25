# VSOL V2802RH RTL9601D bootloader audit

2026-10-25, offline analysis only. No device commands or flash writes were issued. Original backup unchanged.

## Input and verified layout

- `/home/ali/Desktop/v2802rh-full-backup.bin`: 4,194,304 bytes; SHA-256 `4cc28e901cd311806a9103e61d8c7bd8fe32ac103ee5643db504b6a5b2d06086`.
- Reset word at flash offset 0: MIPS big-endian jump to `0x9fc0d070`.
- Preloader region extracted as flash `[0,0xf210)`. This extraction includes code/data/padding, not a claim that every byte is executable.
- Standard 64-byte legacy uImage header at `0xf210`, payload at `0xf250`.
- Image name `U-Boot 2011.12.NA for rtl9602C`; the board prints RTL9601D. The generic image name is not grounds to substitute arbitrary RTL9602C code.
- Header load and entry: `0x80d80000`; MIPS architecture; firmware image type; LZMA compression.
- Compressed payload 71,374 bytes; complete uImage 71,438 bytes. Header and payload CRC32 independently validate against the actual backup.
- Decompressed payload 189,140 bytes; SHA-256 `97ff7cbbf96e05731717773b3fa487d175649cd066a791c9a03a80d4b4ac9236`.
- Current image ends at exclusive offset `0x2091e`, leaving `0x6e2` / 1,762 bytes before the configured boot partition boundary `0x21000`.
- Redundant environment blocks at `0x21000` and `0x23000`, each `0x2000` bytes. Their big-endian CRC32 values validate over bytes 5 onward; flags are 1 and 0 respectively.

The small slack matters: preserving the current preloader and environment boundary leaves at most 73,136 bytes for the compressed replacement payload including any padding after it, with the existing header at its fixed offset. A replacement should preserve the preloader header layout and must fit both its decompression destination and runtime memory contract. Do not assume an enlarged image or shifted header is supported.

## Examined preloader loading path

Ghidra 12.1.4 analyzed an explicitly constructed MIPS big-endian ELF wrapper at `0x9fc00000`, entry `0x9fc0d070`. 194 identified functions produced pseudocode; discovery is incomplete and relocated low-level calls at `0x9f000000` are not fully mapped in this initial analysis.

Function `0x9fc09df0` corroborates the supplied boot log:

1. Checks the DRAM-ready flag and adjusts a controller bit.
2. Reads load/entry/size from the image at `0x9fc0f210`.
3. Tests legacy uImage magic `0x27051956`.
4. Handles uncompressed data or compression value 3 (LZMA); rejects other compression types.
5. Calls the decompressor for the payload at `0x9fc0f250`, checks its return status.
6. Prints the U-Boot start message, calls handoff helpers, then invokes the image entry with a prepared argument block.

No signature/public-key verification is visible in this examined function. The header/data CRC32 checks above were performed by the analyst; they are not proof that the preloader itself enforces these CRCs. No claim is made that every earlier preloader path, handoff helper, ROM operation or fuse setting has been audited. Hardware-rooted secure boot remains unconfirmed. This is substantially better evidence for a replaceable U-Boot payload than command availability alone, but not a proven cold-boot replacement.

## Replacement scope

Prefer retaining the working Bismarck preloader/DDR training and replacing only the packed U-Boot payload after a platform-correct RAM-chainload test. A newer U-Boot must support this exact MIPS CPU/ISA, UART, clocking, SPI and memory map; the preloader handoff argument also needs inspection. Size-constrained builds may be necessary. A full preloader rewrite is a separate task.

Before an installation image is approved: resolve its exact binary contract and size, verify RAM operation, preserve both environment blocks, and establish reliable external SPI recovery. The existing full backup is recovery material, not proof that a programmer connection works.

## Files

Extracted binaries, synthetic analysis ELF, hashes, Ghidra project, logs and pseudocode are in `/home/ali/openwrt-galachip/tmp/v2802rh/bootloader-audit/`. That workspace directory is ignored and may be cleaned; retain it separately if needed. `exports/preloader.analysis.elf.c` is generated pseudocode, not original source.

REA signature scanning was unavailable because its Binwalk command is not configured. REA also declined this non-R2 MIPS profile. The installed Ghidra was used directly without relabeling the ISA to bypass that restriction. No supplied target code was executed.
