# V2802RH L2 repair build, 2026-10-26

Built and copied to TFTP. Firmware source commit: `3c2bc7c1f9`.
Image revision: `r37655+136-6db25ee835`, Linux 6.18.55.
L2 offload remains disabled by default. The subsequent LAN2 off/on/off
capture demonstrates CPU bypass; disabled-path performance and the rest
of the board validation remain open. Hardware QoS follows the L2 gate.

## Changes and rollback

| Commit | Change |
|---|---|
| `3dbb440172` | Plan and audit before implementation |
| `7815556fd4` | Bound the 16-group planner while retaining 64 backend services |
| `df12bdc9a4` | Test TX timing comments and deferred checkpoint saves |
| `239289fde2` | L2 isolation/withdrawal, VLAN ownership, bridge notifications, readback, FDB cleanup, recovery and producer lifetime |
| `3c2bc7c1f9` | Bindings/counter capture in omci-diag and board test instructions |

The pre-change tag is `codex/v2802rh-before-l2-repair-20261026`, pointing
to the tested-source baseline `def58e7375`. The build tag is
`codex/v2802rh-l2-repair-build-20261026`.

To reverse these five commits while preserving history:

```sh
git revert --no-edit codex/v2802rh-before-l2-repair-20261026..3c2bc7c1f9
```

Each implementation commit can also be reverted separately. Nothing was
pushed or flashed. The tested identity/emulation, advertised resources,
Nokia seeds, AES, IVL, SID preservation, optics and deferred checkpoint
behavior are retained.

## Build and verification

`make -j16 V=s` compiled the kernel; package creation initially hit the
sandbox's fakeroot socket restriction. The same build completed with the
required local packaging permission. Logs are retained in ignored
`tmp/v2802rh/l2-audit/firmware-build.log` and
`firmware-build-packaging.log`.

- All four changed driver sources match those in the prepared kernel.
- Both component objects compiled with the configured MIPS toolchain.
- The complete sanitized OMCI suite passed: 2218 MIB, 1537 recovery,
  33 transport, 25 ownership, 118 lifecycle, 16 proxy, 1054 backend-client,
  25 seed-service and 13 OLT-watch checks.
- Sanitized tests of the actual L2 functions passed 196,608 TCAM match
  comparisons and failures at every one of 109 install and 18 withdrawal
  table-transaction positions, plus isolation, readback corruption, flush
  timeout/retry, VLAN ownership, stale generations, notifications and the
  producer fence. Planner inputs of 1–64 distinct groups and the 18-record
  Nokia mapping passed.
- Kernel uImage header/data CRCs and load/entry addresses passed. The
  decompressed kernel contains the new L2 diagnostics. The rootfs is valid
  SquashFS and its omci-diag exactly matches the updated source.
- The combined firmware image is 3,293,188 bytes, within the 3812 KiB
  image limit. Only the requested RAM-boot kernel/rootfs files were shipped.

The kernel link emitted section-mismatch warnings for unchanged
`gpon_serdes_init` references to init-only SerDes arrays. Those warnings
were not changed or addressed by this L2 work. Host tests use mocked
hardware; forwarding and withdrawal still need the board test below.

## Shipped files

The two requested `cp` commands were executed, and byte comparisons plus
SHA-256 checks confirm the TFTP copies match the build outputs.

| TFTP file | Bytes | SHA-256 |
|---|---:|---|
| `/srv/tftp/v2802rh-kernel.bin` | 1,740,793 | `00035d958ecc3fb4663bc69e9d2b2365543f42583fe780c29f3d48bffa1114fb` |
| `/srv/tftp/v2802rh-rootfs.bin` | 1,549,880 | `21e4d72ee1d61de6a54557b863d361058c7ff87fb70f60175c53dfc6f7721517` |

Previous TFTP files were preserved under ignored
`tmp/v2802rh/l2-audit/v2802rh-kernel.before-l2.bin` and
`v2802rh-rootfs.before-l2.bin`. Their SHA-256 hashes are respectively
`e9485a03c65f46e07c77a10809987a8f0dd6dcf8535b24e0dc1f9d1045569623`
and `cfc19ece806ca8d37f4f7a8f9f2920f04ab16e43526032c4b1cc77d679ed7b6d`.

## Board validation

The first LAN2 hardware result and exact counter deltas are recorded in
[L2-OFFLOAD.md](L2-OFFLOAD.md#board-result-2026-10-26). Hardware forwarding
bypasses CPU RX/TX in both directions. Offload-off performance is poor and
its PON bridge-discard counter rises; this is still an unresolved fallback
issue. These documentation updates do not change the shipped images.

Use the off/on/off sequence in [L2-OFFLOAD.md](L2-OFFLOAD.md), checking each
LAN and bidirectional traffic. Capture omci-diag, switch/CPU MIB deltas,
throughput and CPU load before/after each phase. Confirm management and
OMCI remain stable, no duplicates appear and disable restores traffic.
`active=1` alone establishes table installation, not hardware CPU bypass.

A table failure retains isolation and retries; `gated=1` means data can be
interrupted until restore succeeds. Normal disabled state is
`requested=0 active=0 services=0 error=0 gated=0`.
