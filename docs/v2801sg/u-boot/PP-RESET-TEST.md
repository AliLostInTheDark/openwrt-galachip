# SRAM PP-reset test — 2026-10-23

Hardware execution completed below. This test changes volatile packet-engine state;
it does not initialize DDR or write clocks/flash. Fiber must remain disconnected.
After reset, old packet tables cannot be assumed valid. Recovery/reinitialization
is a normal power-cycle through the installed stock bootloader, with the button
released and autoboot interrupted. Do not attempt network commands in the RAM
monitor after the probe. Do not use the button-triggered upgrade path.

## Artifact and offline checks

`v2801sg-sram-pp-reset-4647d6a5.bin`: 932 bytes (`3a4` hex), CRC32 `6d94aaba`,
SHA256 `4647d6a5024cc8d30304cd89c65e4ccbea1b20a86c2529056f5c689e15c4f635`.
Staged in `/srv/tftp/`. Build `sram-engine-probe.S` and `.c` with
`-DZX_ENGINE_RESET`, link with `sram-ddr-recovery.lds`. Result area starts
00200e00, beyond the image and inside the backed-up 4 KiB SRAM prefix.
No unresolved symbols or data/BSS. Models and disassembly are retained under
`/tmp/v2801sg-pp-reset-probe/`.

The entry and preconditions are the same as the passing engine-control probe:
disabled CPU caches/MMU, SRAM stack, interrupts masked, both MAC RX/TX off,
initialized BMU/DMA and stable empty ownership. The probe performs the tested
DMA/BMU disable and readback, then writes the stock mask 000fffff to 92144048.
It samples 9214404c with a one-million-iteration ceiling. It records pre/post
control, status and table bases, keeps BMU/DMA disabled, and returns. Errors
before reset restore saved controls; after reset they never re-enable them.

Source-path host tests cover busy MAC/high-half queues, pre-reset rollback,
the exact reset request, bounded status timeout and no post-reset enables.
These are mocked-MMIO tests. A status register already high before the request
can satisfy the poll immediately; rc=0 is only the status-mask/readback outcome,
not proof of a newly observed acknowledgement transition or global AXI idle.

## Manual commands at the current RAM monitor

```text
tftp 0x40500000 v2801sg-sram-pp-reset-4647d6a5.bin
crc32 0x40500000 0x3a4
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
cp.b 0x40500000 0x00200000 0x3a4
crc32 0x00200000 0x3a4
dcache off
icache off
go 0x00200000
md.l 0x00200e00 0x13
```

Require 932 bytes, CRC 6d94aaba in both payload locations before `go`. Record
the SRAM backup CRC (previously d11f7b32). No UART output is emitted inside
the probe. If it returns, capture all result words before restoring SRAM:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
```

Require restored CRC to equal the backup. Then stop and send the transcript.
No ping/TFTP/Linux/DDR command after this probe. If it hangs, stop until a
physical power-cycle can be performed at the desk.

## Result record (word offsets from 00200e00)

| Offset | Meaning |
| --- | --- |
| 00 | 50525331 after entering the reset phase; otherwise 454e4731 |
| 04 | signed result: 0 sampled status mask, -16 refused, -5 readback/ownership error, -110 timeout |
| 08 / 0c | saved DMA / BMU |
| 10 / 14 | disabled DMA / BMU before reset |
| 18 / 1c | final DMA / BMU, deliberately disabled after reset |
| 20 / 24 | PP reset request / status before reset |
| 28 / 2c / 30 | BPT / ring / buffer bases before reset |
| 34 / 38 / 3c | request / status / poll iteration at completion |
| 40 / 44 / 48 | BPT / ring / buffer bases after reset |

If entry rejects cache/MMU state, it returns -1 without initializing the record.
If reset was never reached, words after 1c are not initialized by this run.
Passing this test does not permit DDR reinitialization. First assess register
effects and verify stock restart/network recovery in a separate boot.

## User hardware result — 2026-10-23

Transfer size 932 and CRC 6d94aaba matched in DRAM and SRAM. The application
returned rc=0 with reset-phase magic 50525331. Saved DMA/BMU were
001f6564/1; disabled and final values were both 00106564/0. PP request/status
were 0/001fffff before and after the request. The poll exited with n=232 hex
(562 zero-based loop index). Because n was nonzero, earlier post-request
status samples failed the mask condition before the final matching sample;
the individual intermediate values were not retained. This provides evidence
of post-request status activity, not a calibrated reset duration or an
independent AXI-idle signal.

BPT/ring/buffer base registers remained 41800000/41900000/41000000. Unchanged
bases do not establish retained table contents or reusable queue state.
The SRAM prefix restored to its saved CRC d11f7b32. No network or DDR test
was reported after the reset. Next: physical power-cycle, normal stock startup
with button released, interrupt autoboot, and verify management recovery.

The user subsequently power-cycled, reached the normal March 2025 stock
U-Boot prompt through the autoboot countdown, and successfully pinged
192.168.1.100 at 1000M full-duplex. This completes physical power-cycle
recovery verification for this PP-reset experiment. It does not demonstrate
warm table reinitialization or DDR reset safety.
