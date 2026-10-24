# ZX279125 U-Boot development status

Start with [UBOOT-REPLACEMENT-JOURNEY.md](UBOOT-REPLACEMENT-JOURNEY.md) for the consolidated 2026-10-23 handoff, exact current prototype identities, source provenance and hardware-versus-model verification boundaries. Older checkpoints below are historical.

[DDR-PATH-REVIEW.md](DDR-PATH-REVIEW.md) traces the vendor's actual SRAM DDR
call path and explains why the current monitor still needs a separate first
stage: its `s_init` is empty and `dram_init` only reports a configured size.

The latest compact base build is 232584 bytes, leaving 29560 bytes within
256 KiB for a future combined layout. The tested RAM chainload remains
byte-identical. [ROM-LOAD-CONTRACT.md](ROM-LOAD-CONTRACT.md) records the current
header analysis, hashes and the unresolved ROM/cold-DDR requirements. Earlier
larger build sizes below are historical checkpoints.

The latest [early-startup source checkpoint](EARLY-BOOT.md) adds offline-verified
DDR/SPI loading primitives and a smaller monitor with the upper RAM bank
removed. These primitives are not enabled in the running chainload path.

Use [CHAINLOAD.md](CHAINLOAD.md) for the separate RAM-only test variant and
exact raw-binary transfer/jump commands. It disables persistent environment
storage, saveenv, automatic boot and automatic SPI-flash unlocking.

The installed bootloader reported on 2026-10-23 is still the vendor U-Boot
2013.04 dated March 7, 2025. This package is a separate, experimental port;
its successful compilation does not explain or resolve the current Linux
Ethernet failure, and does not establish readiness for flash installation.

## PPE/DMA ownership follow-up, 2026-10-23

[PPE-DMA-MEMORY.md](PPE-DMA-MEMORY.md) records the subsequent cursor ownership,
fault handling, external-FDT handoff and Linux diagnostics changes. All four
monitor variants and the Linux module build; hardware verification of this
revision remains pending. The DMA reservation is unchanged. An appended-DTB
kernel does not receive the new external-FDT fixup.

## Ethernet source repairs, 2026-10-23

- Initialize the driver's ring pointer before cold-start `memset`; it was
  otherwise NULL until after cold initialization returned.
- Restrict cold initialization to the captured reset fingerprint. A depleted
  free-buffer pool or an unfamiliar running layout must not trigger table reset.
- Accept only the established buffer/BPT/ring bases. Require a valid retained
  software consumer for every adopted queue instead of guessing index zero.
- Order consumer publication before buffer return. Never return an invalid
  descriptor buffer number to the BMU, and propagate release failures.
- Bound TX length to the available buffer payload and report link-down after
  the cold-start link wait rather than retaining the earlier successful result.

The local-delivery comment no longer claims that two overlapping, incompatible
ONU MAC layouts are programmed simultaneously. The implementation uses the
ONU slot at global offsets 0/4 and the management comparator at 0x40/0x44,
matching the earlier Linux RAM-boot evidence.

## Vendor evidence and open handoff requirements

The supplied `V2801SG_Backups/v2801sg-mtd1.bin` has SHA256
`d286f31239edd5ff9ed5843da41148da869a68a5f72abca11e8b6ec576235c8a`.
Raw ARM instructions at linked addresses `0x41f1f228..0x41f1f448`
read sixteen full count words, descending from `0x9216413c` to `0x92164100`.
The package preserves that vendor U-Boot behavior. This is different from
the stock Linux poller's packed halfwords in eight words; the U-Boot view
still requires explicit queue 8..15 testing and is not a general register-map
claim. Descriptor release at `0x41f1eb10..0x41f1eba4` confirms the mailbox
command and source/queue flag encoding.

The vendor bootloader's relocated software cursor table is `0x41fef75c`.
This port keeps its own table at `0x41840000`. A Linux DT targeting only the
vendor table cannot silently adopt this port's table. Explicit cursor handoff
and memory reservation compatibility must be implemented and tested before
booting that kernel from this bootloader; guessing empty consumers is unsafe.
Cold-start, warm adoption, repeated network commands, and sustained bidirectional
traffic all require new hardware validation after these repairs. No hardware
or flash operation was performed for this source review.

## Build verification

`make package/boot/uboot-zte/compile -j4 V=s` completed successfully after
applying the modified package patch. Artifacts:

- `bin/targets/zte/zx279125/u-boot-vsol_v2801sg/u-boot.bin`: 282376 bytes,
  SHA256 `3a82871d54f694e8286a5b5e539afdab486bae68de4c477282997df254e87969`.
- `bin/targets/zte/zx279125/u-boot-vsol_v2801sg/u-boot.img`:
  SHA256 `07fa5af616ef0a2101d200ee6f61c204d9a2b79698a56a59afab03b35a35a56d`.

The raw binary exceeds the current 256 KiB U-Boot partition by 20232 bytes.
It is not a drop-in flash replacement; size and boot handoff must be resolved
before installation is considered.

The 2026-10-23 rebuild after the cold UART initialization and DDR MMIO source
changes produced a 282496-byte base binary (SHA256
`51b04460313b8adc5a8a649e7187033b46309259541d2937d302fb673a979375`),
20352 bytes over the partition. The RAM-chainload binary remained byte-for-byte
identical to the tested 232480-byte `7f1be111` image. See
[WHOLE-BOOTLOADER-AUDIT.md](WHOLE-BOOTLOADER-AUDIT.md) for the source comparison
and unresolved cold-start requirements.

The build log is `/tmp/zx279125-uboot-review-build.log`. Existing host
`#include_next` warnings and the ignored install-directory removal error do
not constitute runtime validation. These hashes identify this build only.
