# PPE/DMA ownership, memory and Linux handoff

Checkpoint: 2026-10-23, source/build verification after the U-Boot journey handoff.
No UART commands, board resets, flash operations or live DMA changes were performed in this pass.

## Findings and implemented changes

The source monitor keeps sixteen queue base/consumer pairs at `0x41840000`.
Stock U-Boot keeps its verified relocated table at `0x41fef75c`. Linux's
previous validation rejected the source monitor's address, while U-Boot
preferred the stock table whenever Ethernet restarted. Those are CPU software
consumers; an empty hardware queue does not imply a zero producer index.

The U-Boot driver now:

- Adopts a retained table once, then retains its own consumers across subsequent
  start/stop cycles. A mismatch between that owned table and its private state
  becomes an ownership fault instead of restarting from an older vendor index.
- Rejects conflicting valid stock/local histories during first adoption. All
  sixteen candidate consumers are checked before publishing any local table.
- Updates only the owned table after descriptor release. It no longer writes
  into the old vendor loader's cursor storage.
- Latches a fault after uncertain BMU allocation, post-allocation TX mailbox
  timeout, invalid buffer numbers/counts or failed RX release/return. It disables
  LAN ingress and rejects further send/receive/start operations. It retains
  possibly owned buffers rather than guessing a free/reset operation.
- Keeps ordinary no-buffer responses retryable when the allocator explicitly
  reports no valid allocation. A pre-allocation busy TX mailbox also leaves the
  allocation state unchanged.

This changes restart behavior and fault handling; it does not reset the shared
packet engine or establish that AXI transactions have drained.

## Explicit OS handoff

`files/board/zte/zx279125/pp_handoff.c` implements the standard U-Boot
`ft_board_setup()` hook, enabled by the platform defconfig. It operates on an
**external Linux FDT**, after normal network cleanup. It requires the Linux
Ethernet node's memory-region phandle to identify the exact 16 MiB no-map
packet reservation under `/reserved-memory`.

If this monitor has used Ethernet, the driver exports its cursor address only
when both MACs have RX/TX disabled, mailbox and BMU operations are idle,
allocation/release accounting balances, all 4096 buffers are free, all queue
count words are zero, the live pool bases match, and all sixteen local cursor
pairs match private state. Faults, partial initialization and a busy engine
block the fixup. No hardware state is changed by this validation.

If this monitor never attempted Ethernet startup, it preserves the supplied
DT's stock cursor address. Linux must still validate the actual hardware at
probe, or initialize only its established reset fingerprint.

The hook publishes `zte,bootloader-cursors = <0x41840000>` for a valid source
monitor handoff. It then restores the physical 32 MiB memory description;
U-Boot itself continues managing only the low 16 MiB and Linux retains the
complete 16 MiB DMA guard.

Linux now accepts either this known local table or a verified stock table in
the existing upper reserved range. It also requires `of_reserved_mem_lookup()`
to confirm that the DT's resource really belongs to the reserved-memory registry,
with matching physical base/size and `no-map`. A normal node merely containing
those properties is insufficient. Probe now checks mailbox/BMU ownership as
well as pool bases and empty queues before adoption.

### Appended DTB limitation

The current OpenWrt image recipes append a DTB to zImage. That tree cannot
receive the bootloader's external-FDT fixup. ARM's decompressor selects a valid
appended DTB even when another DTB was passed in r2 (`arch/arm/boot/compressed/head.S`).
Therefore **supplying a separate DTB to an unchanged appended-DTB image is
insufficient**.

For the next end-to-end RAM test, package a zImage without an appended DTB and
pass a separate, writable Linux DTB with `bootm <kernel> - <dtb>` (or the
corresponding `bootz` path). The module must be rebuilt into that test root.
Do not use U-Boot's own control DT as the Linux DT. Keep the stock-compatible
image path available; automatic image recipe integration is still outstanding.
All new monitor variants, including the base defconfig, now stop at the prompt
with empty bootcmd and bootdelay -1. The earlier base command read a fixed 2 MiB
appended-DTB image and cannot supply the new handoff, so it is removed until a
validated OS boot recipe is available.

An explicit alternate kernel DT naming the local table is another development
option, but it must not be silently used when booting through stock U-Boot.

No new kernel RAM image was produced or loaded in this source pass. The module
and monitor builds below do not claim that the existing RAM images contain them.

## Read-only Linux diagnostics

`ethtool -S lan0` now exposes 45 fields: thirteen engine/accounting fields plus
pending/consumer values for all sixteen queues. They cover free-buffer counts,
allocation/release counts, TX and release mailbox busy bits, BMU requests and
return status, TX buffer reuse, engine/fault state, DMA control and PP auxiliary
base. Reading them performs no engine reset or table rewrite.

These are snapshots, not a global bus-idle acknowledgement. Hardware counters
can advance while sampled and the allocation/release registers are 32-bit.
The names and values are emitted in the same order under the driver's command
lock. Hardware validation of the new statistics remains pending.

## Memory map: established versus unresolved

All intervals below have exclusive ends.

| Interval | Extent / ownership evidence |
| --- | --- |
| `40000000..41000000` | Low 16 MiB managed by U-Boot and available to Linux subject to its kernel/root allocations |
| `41000000..41800000` | 4096 packet buffers × 2048 bytes = 8 MiB, established from stock instructions |
| `41800000..41802000` | 4096 BE16 buffer-pointer entries = 8 KiB |
| `41820000` | Auxiliary base programmed into PP +34; access extent remains unestablished |
| `41840000..41840080` | Local port's 128-byte software cursor table; existing local convention, still requires combined hardware validation |
| `41900000..41a00000` | Sixteen rings × 4096 descriptors × 16 bytes = 1 MiB |
| `41fef75c..41fef7dc` | Verified stock software cursor table, dependent on that loader's build/relocation |
| `41000000..42000000` | Entire 16 MiB region retained as no-map, including unresolved extents and stock state |

Known buffer/ring/table bytes do not account for the complete reservation.
The auxiliary extent and potential overlap with locally chosen software
storage have not been fully established. The fact that a short LAN transfer
worked does not prove every other queue/auxiliary mode leaves that storage alone.

Consequently this pass reclaims **zero bytes** from the DMA guard. It also does
not shrink the buffer count, ring sizes or optical control queues. Doing so
requires a verified register contract and producer/consumer accounting, not
just changing allocation constants. U-Boot's reported 16 MiB does not mean the
physical device has lost half its RAM.

## Verification performed

Local evidence is retained in ignored `tmp/v2801sg-ppe-handoff-review/`:

- Before-patch/source snapshots, extracted actual U-Boot driver and C tests.
- Tests of the actual start/release/stop/handoff functions with simulated MMIO:
  sixteen nonzero consumers survive restart; stale vendor data stays untouched;
  a queue-15 conflict leaves the complete local table unpublished; owned-table
  corruption faults; BMU and mailbox failure injections stop further commands;
  invalid allocations never address packet memory; a release timeout never frees
  the buffer; busy MACs, mailboxes, BMU and queue words block OS handoff.
- Tests of the actual board fixup and extracted upstream memory helper linked
  to host libfdt: correct local cursor, physical memory and unchanged no-map
  guard; unused-engine stock fallback; fault/busy rejection; invalid region,
  phandle or Ethernet node rejection.
- Linux Ethernet module rebuilt and linked against prepared Linux 6.18.55 with
  `W=1`. The configured `CONFIG_MODULE_STRIPPED=y` produces the existing modpost
  missing-description warning despite the source's MODULE_DESCRIPTION; the
  toolchain wrapper also warns when STAGING_DIR is not set. Neither was a
  compile/link failure.
- U-Boot base, stopped chainload, UART-init and dump variants rebuilt through
  the OpenWrt package. `CONFIG_OF_BOARD_SETUP=y` is enabled in the new monitor.

Scripts/models do not prove real DMA ordering, complete global shutdown,
queues 8..15 on physical hardware or a cold boot. U-Boot retains the full-word
vendor polling view, while Linux uses the stock kernel's packed halfwords;
this difference remains explicitly unresolved for generalized hardware use.

## Next verification work

1. Freeze the new monitor, module, external DTB and unappended kernel into a
   separately named RAM-test set, with hashes and load bounds checked below
   the packet reservation. Include the changed module in the test root.
2. RAM-chainload the stopped monitor and repeat ping/TFTP plus stop/restart
   checks. Do not rerun a retired warm DDR payload.
3. Boot the prepared kernel with the external DTB, verify the published cursor
   property and all sixteen consumers, then measure BMU balance and queue
   progress through bidirectional transfers and interface down/up.
4. Establish the auxiliary table's actual extent from exact vendor evidence
   before relocating tables or considering any memory reclamation.
5. Validate the physical high-queue count representation and shared PON control
   ownership separately. Fiber remains disconnected for LAN-only work.

The older combined cold-stage image embeds the old monitor and its compiled-in
CRC. **Do not splice a newly built monitor into that image.** Regenerate the
payload header, stage and manifest and rerun the complete-path audit when a
new cold candidate is deliberately assembled. Nothing in this pass installs a
replacement bootloader or establishes a working cold recovery path.

### Frozen monitor artifacts from this pass

These files are built and software-tested only; none was staged for TFTP or executed. The manifest also binds the Linux module and changed source hashes.

| Variant | Bytes | CRC32 | SHA256 |
| --- | ---: | --- | --- |
| `vsol_v2801sg` | 234776 | `ef777b1a` | `6f00b372c39d9306891d78d5f29e3ceb161965ee309ef6e9943afb9025b035bb` |
| `vsol_v2801sg-chainload` | 234736 | `d1ca0f53` | `8daa829021d62f3cac4caf2b714e22b9ced00fdfeb265ee5382a98a5afebfea4` |
| `vsol_v2801sg-uart-test` | 234792 | `e1a6d58b` | `4dd15434ba14c220b52f45325516499dcd86a0e223862706f225927b3d01a5e8` |
| `vsol_v2801sg-dump` | 236440 | `4aa93417` | `a7a9a2d2509ed29b0dcbaacec7c5b7b8a2757f05c0fbae2cd0aeb784803851c0` |

Manifest and copied monitors: `tmp/v2801sg-ppe-handoff-review/manifest.json` and its per-variant directories.
