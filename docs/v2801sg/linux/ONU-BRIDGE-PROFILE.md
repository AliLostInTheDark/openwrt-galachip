# VSOL V2801SG bridge-only profiles

## Policy, 2026-10-23

Subscriber traffic belongs in the hardware PON-to-LAN bridge. IPv4 on br-lan
is retained only for administration. Removing INET altogether would also
remove SSH and LuCI. The fresh board configuration now has one bridge with
lan0 and pon0, no WAN/WAN6 interfaces, and IPv4 forwarding disabled.

The ZTE target has no local IPv6 stack, netfilter/NAT, advanced routing,
multicast routing, EPON/OAM stack or swap. Removing local IPv6 does not remove
Ethernet forwarding of IPv6 frames. The shared OpenWrt IPv6 default is changed
only for TARGET_zte_zx279125; other targets retain the existing default.

This is the intended bridge-only configuration, not proof of working optical
subscriber forwarding. The current ZTE provider still needs complete hardware
flow/VLAN/GEM provisioning and optical validation. Its pon0 CPU TX drops data;
Linux software bridging is not a substitute for that missing hardware backend.
Laser permission remains off by default. Keep fiber disconnected for LAN tests.

## Memory reductions

- Production seed: 70 selected packages versus 105 in the preceding working
  config. Firewall/iptables/PPP, routing UI, package manager, downloads/TLS
  dependencies, EPON applications and the router-side xPON stats client are
  excluded. The separate RAM LAN-test seed selects 43 packages.
- LuCI is selected as individual management modules, avoiding the collection
  that pulls firewall/PPP back in. Existing G7 view/menu pruning was already
  present and is retained. The G7 iwinfo/QR dependency exclusions now also apply
  to ZTE in the LuCI feed's network/status package Makefiles.
- Production keeps OMCI, PON administration and the in-process ucode LuCI
  handler. HTTP concurrency is bounded to one request/eight connections,
  keep-alive to five seconds, and the system log ring to 32 KiB.
- BusyBox drops the same 24 applets as the G7, among them the DHCP client,
  `route`, `ifconfig`, `nslookup`, `traceroute6`, `brctl` and interactive tools;
  the management address is static. The
  `u-boot-vsol_v2801sg-stockchain` monitor the sysupgrade image carries is
  selected by default with the device.
- The TCP source-port perturbation table changes from order 16 to order 8:
  256 KiB becomes 1 KiB. Kernel crash/kexec/debugfs and swap are disabled.
  Kallsyms/devmem and packet diagnostics remain for this development stage.
- The RAM LAN test omits LuCI, rpcd, uhttpd and optical userspace daemons. It
  retains the kernel GPON provider with laser permission defaulting off.

These are source/configuration savings. Measure live MemAvailable, process RSS
and stability after RAM boot before claiming a runtime memory result. A lean
LuCI profile is not evidence that every concurrent request fits in RAM.

The lean RAM image completed a full build and static image checks. It is
3316056 bytes versus the preceding 4689472-byte image. Its 1069056-byte root
contains no LuCI/rpcd/uhttpd/OMCI daemons, firewall or routing packages. Build
logs are in `/tmp/v2801sg-onu-profile/`. The first sandboxed packaging attempt
failed because fakeroot could not create an IPC socket; the normal build
outside that sandbox completed successfully. No hardware test has run yet.

## DMA memory

Linux reserves the whole top 16 MiB (`41000000..42000000`, no-map) and has
about 11 MiB. A narrower reservation was tried and withdrawn: the engine writes
a descriptor ring at `41a00000` that no bootloader programs, which corrupted
process memory (docs/v2801sg/u-boot/PPE-DMA-MEMORY.md).
Shrinking the pool needs the engine re-initialized with a complete ring map,
as the vendor kernel does (PPE-L2-OFFLOAD.md).

## Selecting a profile

Save .config before changing profiles. For the first LAN recovery RAM boot:

```sh
cp .config /tmp/v2801sg-config-before-profile
cp target/linux/zte/vsol_v2801sg-bringup.diffconfig .config
make defconfig
make -j4
```

For the management UI and OMCI development profile, use
`target/linux/zte/vsol_v2801sg.diffconfig` instead. This profile has been
resolved and audited, but its full image/runtime is not yet validated.
The commands in RAMBOOT-TEST.md refer to the lean RAM test artifact, not this
UI profile. Neither profile authorizes flash installation.

Config defaults apply to a fresh root. They do not rewrite the already
installed overlay's old WAN settings. A compressed RAM boot uses its own
temporary overlay and does not mount the installed rootfs_data.
