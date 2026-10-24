# V2801SG next boot test: UART-only SRAM audit

Prepared 2026-10-23. No hardware execution during this verification pass.
The previous DDR recovery test hung; do not retry either DDR recovery image.
The user has since confirmed that button-at-startup invokes a stock TFTP
upgrade path. Release the button for normal startup, interrupt autoboot over
UART, and confirm the installed stock U-Boot prompt returns. Keep fiber
disconnected and capture the complete serial transcript. Keep `upgrade.bin`
absent from the TFTP directory; that path invokes an installer, not RAM boot.

## Updated RAM monitor with bounded Ethernet shutdown

The built and staged monitor is `v2801sg-uboot-stop-7f1be111.bin`, 232480 bytes
(`38c20` hex), CRC32 `a4072a62`, SHA256
`7f1be11117e6e742cc7f258cd5b9be865f869d75504668088ec2340a2ebcafac`.
From stock U-Boot:

```text
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.1
tftp 0x40400000 v2801sg-uboot-stop-7f1be111.bin
go 0x40400000
```

The installed stock monitor has no `crc32` command, as confirmed by the user.
Require the exact transfer size before `go`; this is a size check, not checksum
verification. The new monitor supports `crc32` for subsequent test payloads.
In the new monitor, record
`version` and `bdinfo`, then set `ethaddr` to `70:b6:4f:01:a6:7a`, `ipaddr` to
`192.168.1.1`, and `serverip` to `192.168.1.100`. Run `ping 192.168.1.100`
three times. Each should return to the prompt without an ownership warning.
Then perform the audit below. Any hang ends testing until recovery at the desk.
Environment is RAM-only and `saveenv` is disabled in this monitor.

## Verified offline

- Reconstructed DDR source matches 35 vendor MMIO writes and three delay
  events in order. This establishes instruction-level agreement, not working
  warm DDR reinitialization or the reset-ROM loading contract.
- The original assembled banner leaves exactly 16 bytes queued when entering
  the loader in the FIFO model, matching the observed truncated prefix.
  Repeated `g` output is not reproduced by this model.
- The UART-only assembled audit drains both messages, restores callee-saved
  registers, SP and CPSR, and never enters the DDR loader. Modeled stuck-drain
  handling returns -110; that test shortens only the modeled poll budget.
- The staged audit and build output have identical SHA256:
  `96f6be2ba8b8c51732507f0425f2b6ed19d8a8b13bd107a404f079447e17bccb`.

Evidence and harnesses: `/tmp/v2801sg-ddr-audit/`,
`/tmp/v2801sg-sram-ddr-recovery/`, and
`/tmp/v2801sg-sram-ddr-uart-audit/`.

## Run only from the RAM-chainloaded 2026.10 monitor

Confirm `ZX279125 #`, `version`, and `bdinfo` first. These commands assume the
existing working RAM monitor with `icache` and `dcache` commands. The stock
2013 monitor's published command list lacks those commands; do not substitute
an unchecked jump from stock. SRAM backup CRC below is the previously observed
value, so retain and report the actual value if it differs.

```text
tftp 0x40500000 v2801sg-sram-uart-audit-96f6be2b.bin
crc32 0x40500000 0x99d
cp.b 0x00200000 0x40510000 0x1000
crc32 0x40510000 0x1000
```

Require transfer size 2461 bytes and image CRC `1f03cb0c`. Previous SRAM CRC
was `d11f7b32`. Keep the backup intact. Then:

```text
cp.b 0x40500000 0x00200000 0x99d
crc32 0x00200000 0x99d
dcache off
icache off
md.l 0x92200000 0x1
go 0x00200000
```

Require SRAM image CRC `1f03cb0c` before `go`. The audit checks CPU cache/MMU
state and MAC RX bit 2 itself. Expected output includes the complete original
banner followed by:

```text
UART drain audit only: DDR loader was not called; returning.
## Application terminated, rc = 0x0
```

The original banner mentions initializing DDR, but this audit returns before
that call. After successful return, restore the saved SRAM prefix:

```text
cp.b 0x40510000 0x00200000 0x1000
crc32 0x00200000 0x1000
```

Require the restored CRC to equal the backup's actual CRC. If output repeats,
stops, or the prompt fails to return, stop testing and retain the transcript;
recover with a power-cycle at the desk. Do not attempt another DDR variant.

## Remaining blocker before another DDR attempt

The updated stop callback disables RX, drains receive descriptors through the
existing release path, checks mailbox/BMU ownership and complete queue-count
words, and requires two idle observations separated by 1 ms before disabling
TX. Timeout or receive failure retains TX and the reserved pool with a warning.
Host tests cover success, busy counters including high-half queues, receive
errors and timeout. These checks do not prove global AXI inactivity.

The default DDR probe now also rejects any nonzero BMU control register and
RX/TX enable on either Ethernet MAC. This deliberately rejects the initialized
engine after normal TFTP operation, even when Ethernet shutdown succeeds.
The UART-only audit is unchanged byte-for-byte and does not need that gate.
There is still no hardware-validated full-engine quiescence sequence. That
must be resolved before retrying destructive warm DDR reinitialization.
The ZX279128S upstream series has different DMA layouts and is not evidence
for applying its reset masks here. UART drain success alone cannot clear this
blocker. ROM entry/copy behavior also remains unverified for power-on boot.

## User hardware result — 2026-10-23

The 232480-byte monitor chainloaded successfully from stock. `bdinfo` reported
one 16 MiB bank, relocation 40fb4000 and stack 40ba37a0. After setting the
factory MAC, all three pings succeeded; no ownership warnings were shown.
The audit transferred 2461 bytes and matched CRC 1f03cb0c both in DRAM and
SRAM. Before execution MAC 92200000 read 020e0880, with RX/TX bits clear.
The complete banner and audit message printed, the application returned rc 0,
and restored SRAM CRC d11f7b32 matched its backup. This establishes the UART
drain/return path and warm management operation on hardware. It does not
establish global DMA inactivity or working DDR reinitialization. No DDR loader
call or flash operation occurred in this audit.
