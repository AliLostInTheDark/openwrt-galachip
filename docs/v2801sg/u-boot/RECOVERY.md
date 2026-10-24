# V2801SG stock startup and upgrade entry

## Observed on the board

On 2026-10-23 the user reported that the button-at-startup attempt produced
the stock March 2025 U-Boot banner, then requested `upgrade.bin` by TFTP from
192.168.1.100 into 0x40600000. Aborting the transfer printed `update version
fail!` and the `downver` usage text. This confirms an upgrade entry path while
the installed bootloader works. It does not demonstrate successful flashing,
image compatibility or recovery after overwriting the bootloader itself.

Keep `upgrade.bin` absent during RAM experiments. No successful download or
installer execution is requested by the current test plan. Normal recovery
from a RAM-only hang means releasing the button, cycling power, and stopping
normal autoboot over UART. Holding the button selects a different path.

## Exact stock binary evidence

Input: `/home/ali/Desktop/MISC-3/V2801SG_Backups/v2801sg-mtd1.bin`, SHA256
`d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a`.
Raw ARM disassembly: `/tmp/v2801sg-vendor-uboot-disasm.txt`.

- At 41f1d564 the startup helper calls the GPIO input reader at 41f1d3ac
  with index 9. While the input is low it calls a delay with argument 250000;
  the comparison against 20 and loop counter establish a repeated hold check.
  No calibrated wall-clock duration has been established here.
- The GPIO reader loads a base of 94404000 from 41f288e0, uses bank stride
  0x40, reads offset 0x14 and tests bit `index & 15`. For index 9 that is
  input 94404014 bit 9. This is an exact-stock lead for the button pin,
  not permission to transplant the ZX279128S GPIO map.
- Startup at 41f0f644 calls the hold-check helper. A low result enters the
  branch at 41f0f650 and dispatches `downver version` at 41f0f664. The text
  is stored at 41f263e0. This agrees with the user's transfer transcript.
- The command table at 41f292f4 binds `downver` to 41f05484. Its `version`
  argument branch at 41f055c0 calls the update helper 41f0513c with selector 4.
  After success it can invoke the flash-device callback with offset 0x30000
  and size 0x10000 at 41f05638..41f05650. Therefore this command is not a
  read-only TFTP convenience. The complete installer erase/write layout has
  not been audited and must not be inferred from the load address.

This recovery depends on the installed SPI bootloader reaching its GPIO and
network routines. It is not demonstrated mask-ROM recovery and provides no
assurance that replacing flash offset zero would remain recoverable.

## Packet-engine shutdown finding

The stock function 41f1f7b4..41f1f7d4 clears RX/TX bits 2 and 3 in MACs at
921c0000 and 92200000, then returns without polling DMA, mailbox or BMU.
Startup calls it at 41f0f864; network paths also call it at 41f07628,
41f07b70 and 41f07c40. That establishes MAC cleanup, not DDR-safe shutdown.
The vendor DMA helper at 41f1ed90 toggles mask 0x000f0000 in 92164000 based
on its argument, also without an idle poll. It must not be treated as an
AXI-drain acknowledgement.

The replacement monitor now uses conservative bounded ownership checks before
disabling TX. Full packet-engine shutdown across a warm DDR reset is still
unverified; the SRAM DDR diagnostic rejects an initialized BMU instead of
assuming MAC RX-off makes that operation safe. A passing UART-only audit does
not authorize another DDR reset. Source/model evidence for this pass is in
`/tmp/v2801sg-quiescence-review/`; live testing remains pending.

## Follow-up after the successful UART audit

The user's post-audit snapshot shows BMU control 1; counts fb0 + 50 = 1000
(hex), and allocation/release counters both 16d. BMU request 92168010 is zero,
status 921680c0 is 40000111 (busy bit 3 clear), TX/release mailboxes are zero,
and all 16 sampled DMA count words are zero. MAC0 is zero and MAC1 is
020e0880 (RX/TX disabled). DMA control remains 001f6564; PP request/status
92144048/4c are 0/001fffff. This is an initialized engine with balanced
ownership, not an observed global-idle acknowledgement.

Further exact stock tracing identifies these independent controls:

- 41f1e93c writes zero to BMU control 92168000 and returns immediately.
  41f1e950 writes one. No wait, barrier or bus-idle test is present in either.
- 41f1ed90 preserves DMA control outside mask 000f0000 and sets/clears that
  mask based on its argument. Applied arithmetically to the snapshot, the
  clear operation would produce 00106564. That value has not been written.
- The stock initialization routine at 41f1edcc writes 000fffff to PP +48,
  calls a delay with argument 1000, clears mailbox tables at 41f1ecf0,
  enables the DMA mask via 41f1edb0, configures the classifier, initializes
  the BMU, and enables it at 41f1ee10. It is initialization, not a proven
  reversible shutdown procedure. The raw routine does not poll PP +4c here.

A direct-call search of this stock image found the BMU-enable and DMA-enable
calls above, but no direct branch-and-link to the BMU-disable function.
Indirect calls are not excluded by this search. Consequently, disabling BMU
or toggling a reset bit cannot yet be described as the vendor's validated
warm DDR shutdown sequence. In particular PP status 001fffff with request
zero is not a fresh acknowledgement of a new reset request.

Next hardware experiment, if prepared, must isolate engine shutdown from DDR:
retain code/stack in SRAM, save control values, require the recorded empty
ownership state, apply only individually traced disable operations, read back,
then restore and verify network operation before considering a reset test.
Do not combine an unvalidated engine reset with DDR initialization. No new
hardware commands or DDR retry are authorized by this evidence alone.

## Full-flash TFTP backups (2026-10-24)

Three complete 8 MiB reads, taken from the RAM-chainloaded `dump` monitor
(`v2801sg-uboot-dump-0797b5d5.bin`) with `sf read` in 1 MiB chunks and
`tftpput`. Every chunk was accepted only when the received file's size and
CRC matched the board's `crc32`; no chunk needed a retry.

- All three runs are identical: SHA256
  `d0a11fce785041f8c34029de02decaa3b173b3dc2b5f8d0a2028d8459752cd47`,
  CRC32 `f90c92e4`.
- Against the 23 October capture: boot code and environment
  (`0x00000..0x3ffff`, CRC `34d4cc5a`, env `eeae6842`), `others`, `tagparam`
  and `usercfg` are identical. 1701 bytes differ, all inside `rootfs_data`
  (`0x410000..0x77ffff`), the installed OpenWrt's overlay.
- Against the original backup: boot code and `others` identical; the
  environment differs by the known seven bytes; firmware, `tagparam` and
  `usercfg` reflect the installed system.

Copies: `/home/ali/Desktop/MISC-3/V2801SG_Backups/v2801sg-full-flash-8M-20261024.bin`
with `SHA256SUMS-20261024.txt`; all runs and chunks in
`tmp/v2801sg-nor-backup-20261024/`.

These are faithful backups, but not a recovery path: restoring through TFTP
needs a bootloader that still runs. If the boot partition is damaged, only an
external programmer (the SOIC8 clip) can write it back, so the cold test still
waits for a demonstrated clip readback and restore.
