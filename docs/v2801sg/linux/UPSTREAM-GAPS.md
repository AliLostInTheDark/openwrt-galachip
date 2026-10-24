# ZTE target: gaps before an upstream OpenWrt submission

Review date: 2026-10-24, against the `v2801sg-zte-pon` branch.

## Fixed

- **Vendor prefix:** the board is `vsol,v2801sg` everywhere; patch 911 adds
  `vsol` to `vendor-prefixes.yaml`. Images also list `v-sol,v2801sg` in
  `SUPPORTED_DEVICES` so boards on older images can sysupgrade.
- **DT bindings:** schemas now exist for the board/SoC (`arm/zte.yaml`), CRM,
  timer, UART, SPI flash controller, GPIO, I2C and PON, besides Ethernet and
  MDIO. The unregistered `semtech,gn25l95` compatible is dropped. The schemas
  are parsed and cross-checked against the DTS; `dt_binding_check` has not run
  (dtschema is not installed).
- **Hardcoded addresses:** the Ethernet driver takes the pool base from
  `memory-region` 0 and every table from fixed offsets in it; the cold-start
  register ranges are named `reg` entries.
- **Coding style:** `checkpatch` reports only a false positive in
  `net/xpon/omci/core.c`. The vendor-ID default check in `identity.c` was a
  real bug (a pointer compared with a compound literal).
- **Shared host key:** `base-files/etc/dropbear/dropbear_rsa_host_key` is out
  of the tree, so images no longer ship one private key.
- **U-Boot package:** patch 001 is now a seven-patch series (verified to
  produce an identical tree). The chainload, dump and UART-test variants are
  opt-in; selecting the V2801SG selects the stockchain monitor the image needs.
- **Netlink ABI:** YNL specs in `Documentation/netlink/specs/xpon.yaml` and
  `omci.yaml`, generated from the UAPI enums, the attribute policy and the
  attribute writers.
- **Notes:** design and bring-up notes moved from `target/linux/zte` and
  `package/boot/uboot-zte` to `docs/v2801sg/`.

## Open

- **New UAPI / genetlink** (`xpon`, `omci`): now specified, but a new
  subsystem will still be questioned. `XPON_CMD_GET_CAPS` is declared without a
  handler, and the userspace packages carry private copies of `omci.h`.
- **`arch/arm/mach-zx/zx279125.c`:** check whether a generic DT machine plus
  drivers covers it.
- **U-Boot side** (`files/board`, `pp_handoff.c`, `zx279125-pp.h`) still uses
  fixed pool addresses.
- **Development aids:** `ramroot.mk`, the RAM-test profile and the cold
  first-stage tooling.
- **Device recipe:** the long `DEVICE_PACKAGES` removal list could move to the
  profile or target defaults.

## Functional gaps reviewers will ask about

- PON data path: no hardware forwarding; `pon0` CPU transmit drops data
  (PPE-L2-OFFLOAD.md).
- No OLT/OMCI interoperability test recorded.
- Initramfs and sysupgrade from OpenWrt not tested with the loader layout.

## History

Many commits are experiments and their reverts. A submission should be a
re-cut series: SoC drivers, DTS, target, image, then the board.
