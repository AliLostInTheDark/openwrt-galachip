# V2802RH: firmware prepared for the chainloaded U-Boot

Built on `vsol-v2802rh` at source commit `ec6e778e1e` on 2026-10-26.
Image revision: `r37655+155-6db25ee835`.

- `14b9e384ac`: reserves U-Boot and its environment behind the firmware;
  applies the isolated layout commit `158d0dac20` to the current device branch.
- `ec6e778e1e`: removes the broken serial autologin default and adds an
  exact-match migration back to `::askconsole:/usr/libexec/login.sh`.
  The user approved this repair. Custom tty entries are preserved.

The user verified U-Boot `67fe7286`: compressed RAM chainload works,
`button reset` reports off/on/off, and holding reset while entering `boot`
selects HTTP recovery. The final DMA halt was the user's Ctrl-C.

## Validation

Full `make -j$(nproc) V=s` passed. The first sandboxed attempt could not
start fakeroot's socket; the build passed outside that restriction. Final
log: `/tmp/v2802rh-layout-console-build.log`.

Verified the actual kernel's header/data CRCs, LZMA decompression, embedded
DTB and all partition ranges/read-only flags. Firmware remains at 0x27000,
ends at 0x3c0000, and has size 0x399000 (3772416 bytes). U-Boot occupies
0x3c0000–0x3df000, its environment 0x3df000–0x3e0000; both are read-only
to Linux. Stock boot, both stock environments, config and OSK remain
read-only with their original ranges.

The size guard accepts an image exactly at the partition limit and rejects
limit+1 and a non-uImage. The console migration repairs the legacy entry,
preserves a custom tty and is idempotent. The built squashfs contains the
standard inittab and repair script, with no old autologin script. Package
manifest is unchanged. Sysupgrade metadata identifies `vsol,v2802rh` and
`realtek-luna/taroko`; raw firmware is the exact sysupgrade payload prefix.

Existing modpost warnings reference `gpon_serdes_init` and two `__initconst`
tables. The runtime full-SerDes-reinit option is already disabled by default;
this work changes no GPON driver code. The user subsequently verified the
new running version, partition layout and console entry as recorded below.

## Artifacts

All files are under `bin/targets/realtek-luna/taroko/` with prefix
`openwrt-realtek-luna-taroko-vsol_v2802rh-squashfs-`.

| Suffix | Bytes | SHA-256 |
|---|---:|---|
| kernel.bin | 1743215 | fdbcbf5ed947df06e05110743e1c41defefd8d8db415ccc781663252f5d3fee4 |
| rootfs.bin | 1547892 | b730d596511bf378816905715d41970a4d45e7438361c3813c2a08bc79bbc6a6 |
| firmware.bin | 3293188 | 5e0f246ee0b6c8f6f54807609d46f921687155c81fff4954c665777f3ebb6287 |
| sysupgrade.bin | 3293460 | 9e4a716d492d40b8d4142838d2656d4382f3c6ecdfc4ff998a9d4c86704d121e |

Sysupgrade is 478956 bytes below the firmware limit. Config backup space
must also fit when upgrading while preserving settings.

## Next operation and rollback

The user installed the firmware and reported running revision
`r37655+155-6db25ee835`, command line `console=ttyS0,115200`, firmware
size `00399000`, U-Boot `0001f000`, U-Boot environment `00001000` and
rootfs_data `00075000`. Inittab contains
`::askconsole:/usr/libexec/login.sh`. The required layout gate has passed.

The proposed installation of `67fe7286` was deferred when the user asked
to validate all recovery/update paths first. The subsequent audit produced
U-Boot `4447ad1c` and companion OpenWrt +158 with additional guards.
See [the recovery test checklist](UBOOT-RECOVERY-TESTS.md) for current files,
hashes, host results and RAM tests. These builds have not been installed;
the user's verified +155 remains the running baseline. Erase/programming
and environment saves still need explicit approval before execution.
No board access or flash write was performed by the agent.

Rollback source tag: `codex/v2802rh-before-uboot-layout-20261026` at
`069f27af99`. Original config/images are saved in
`tmp/v2802rh/uboot-chainload/pre-layout-build/`; previously served images
in `tmp/v2802rh/uboot-chainload/pre-layout-tftp/`.

The original 4194304-byte backup was rehashed:
`/home/ali/Desktop/v2802rh-full-backup.bin`, SHA-256
`4cc28e901cd311806a9103e61d8c7bd8fe32ac103ee5643db504b6a5b2d06086`.
This confirms the file, not that external programmer recovery was tested.
