# V2802RH default-enabled L2 build, 2026-10-26

Source commit: `c0bea668e5` on `vsol-v2802rh`.
Image revision: `r37655+139-6db25ee835`, Linux 6.18.55.

Following the LAN2 CPU-bypass capture, the user requested default enablement
on this device branch. V2802RH now requests L2 offload during driver init;
the taroko bridge defaults set `igmp_snooping=0`. The existing bridge,
service, table-ownership and isolation checks still decide activation.
The runtime sysfs switch remains available. Preserved network configuration
can still prevent activation, for example by explicitly enabling snooping.

## Verified build and delivery

`make -j16 V=s` completed successfully. The existing sanitized L2 checks
passed 196,608 TCAM comparisons and injected failures at 109 install and
18 withdrawal transaction positions, plus isolation, VLAN ownership,
readback, flush retry, generation and teardown-producer checks. These are
host checks with mocked hardware; default activation after boot still
needs the board test below.

The prepared kernel source matches the committed driver. uImage header
and data CRCs, load/entry at `0x80000000`, kernel diagnostic strings,
SquashFS format and the rootfs network defaults passed verification.
The combined firmware is 3,293,188 bytes, within the 3812 KiB limit.
The known SerDes init-section warnings remain, as do a module-description
warning and a submake jobserver warning.

Both requested TFTP copy commands completed. Byte comparisons and SHA-256
confirm the shipped files match this build:

| File | Bytes | SHA-256 |
|---|---:|---|
| `/srv/tftp/v2802rh-kernel.bin` | 1,740,068 | `0a33e42b9b97ad87aeac7a99610f640eb542382d0451a6acf972752da1e62d2f` |
| `/srv/tftp/v2802rh-rootfs.bin` | 1,549,900 | `fd2e39c000852609e93c937501e92e42518ec581261d4355db26ebbcab860cf8` |

Build log and verification metadata are in ignored
`tmp/v2802rh/l2-audit/default-enable-build.log` and
`default-enable-images.json`. The previous tested TFTP pair is preserved
in `tmp/v2802rh/l2-audit/before-default-enable-20261026/`.

## Next board check and rollback

Boot this kernel/rootfs pair without a manual enable command. After O5
and provisioning, expect `requested=1 active=1 services=2 error=0 gated=0`
from `/sys/class/net/pon0/device/l2_offload` for the tested two-service setup.
Check management access and traffic; repeat the counter comparison on LAN1.
Recovery/service-change tests, poor disabled-path performance and hardware
QoS remain open, as recorded in [L2-OFFLOAD.md](L2-OFFLOAD.md).

Disable until reboot:

```sh
echo 0 > /sys/class/net/pon0/device/l2_offload
```

The rollback tag is `codex/v2802rh-before-l2-default-20261026` at `b1bdf6fbbf`.
To revert the default change while preserving history:

```sh
git revert c0bea668e5
```

The build tag `codex/v2802rh-l2-default-build-20261026` identifies the source.
No push or flash operation was performed.
