# VSOL V2801SG LAN recovery RAM test — 2026-10-23

Use the existing March 2025 stock U-Boot. Keep fiber disconnected. This image
is a LAN recovery test with IPv4 administration, no local IPv6, LuCI, rpcd or
optical userspace daemons. The kernel GPON provider remains present with laser
permission defaulting off. Hardware PON-to-LAN forwarding is not tested here.
No flash/erase/upgrade/saveenv command is part of this procedure.

## Exact artifact

`bin/targets/zte/zx279125/v2801sg-lan-recovery-469a2e9c.uImage`

- Size: 3316056 bytes.
- SHA256: `469a2e9c2c8c951c841f3aef1e5b574d9aecd8567f511e2377deae1937a87a9b`.
- Full build passed; final log: `/tmp/v2801sg-onu-profile/ram-build-last.log`.
- Header/data CRCs passed; load/entry 40008000.
- Loaded at 40600000, file ends at 40929958, below the 41000000 packet pool.
- Kernel/BSS end 40647ed8; a conservative relocation bound is 40991830.
- Appended DTB, 23-entry bootstrap CPIO and embedded 1069056-byte SquashFS match
  the staged build. The root contains the repaired driver and fresh bridge defaults.
- Verification details: `/tmp/v2801sg-onu-profile/image-verification.json`.

## PC: stage the file

The dedicated NIC must already have 192.168.1.100/24 and the existing TFTP
server must serve /srv/tftp. These commands only copy/check the image:

```sh
sudo install -m 0644 /home/ali/openwrt-galachip/bin/targets/zte/zx279125/v2801sg-lan-recovery-469a2e9c.uImage /srv/tftp/
sha256sum /srv/tftp/v2801sg-lan-recovery-469a2e9c.uImage
```

## U-Boot UART

Reset, interrupt autoboot, then run:

```text
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.1
tftp 0x40600000 v2801sg-lan-recovery-469a2e9c.uImage
setenv bootargs console=ttyS0,115200n8 mem=32M panic=0
bootm 0x40600000
```

Proceed to bootm only after TFTP succeeds with 3316056 bytes. U-Boot should
report a valid kernel checksum. Do not run the old bootcmd or any installation
alias for this test. The setenv changes above are temporary; do not save them.

## OpenWrt UART: collect the baseline

Wait for the root shell and br-lan forwarding/link-up messages:

```sh
cat /etc/openwrt_release
grep -E 'MemTotal|MemAvailable|MemFree|Slab|SUnreclaim' /proc/meminfo
ip link show
ip addr show br-lan
uci show network
sysctl net.ipv4.ip_forward
ethtool lan0
cat /proc/interrupts
dmesg | tail -n 80
```

Expected: factory LAN MAC, 192.168.1.1/24 on br-lan, lan0 carrier,
forwarding=0, bridge ports lan0 and pon0, and no WAN/WAN6 sections. The adopted
consumer need not be zero: preserve the value left by stock U-Boot. Stop if
there is an invalid handoff, DMA ownership fault, OOM or panic. If the whole
device becomes unresponsive, stop testing and leave recovery to the user.

## PC: test reception and latency

Run the short test first. If it loses packets or has multi-second latency,
stop and collect UART output instead of starting a long flood:

```sh
ping -I enx6c1ff7c6fccf -c 20 -W 1 192.168.1.1
ping -I enx6c1ff7c6fccf -c 1000 -i 0.01 -s 1472 -M do 192.168.1.1
```

For the SSH listener, use a bounded banner probe without changing credentials
or PC host-key records:

```sh
python3 - <<'PY'
import socket
with socket.create_connection(('192.168.1.1', 22), timeout=3) as s:
    s.settimeout(3)
    print(s.recv(256).decode(errors='replace').strip())
PY
```

## OpenWrt UART: test the reverse path and collect counters

```sh
ping -c 20 -W 1 192.168.1.100
ip -s link show lan0
ip -s link show br-lan
cat /proc/interrupts
grep -E 'MemTotal|MemAvailable|MemFree|Slab|SUnreclaim' /proc/meminfo
dmesg | tail -n 100
```

Require no duplicate replies, multi-second queueing, new driver errors or OOM.
Compare interrupt/counter progress with traffic rather than expecting every
shared IRQ to be a LAN packet. Host firewall policy can affect reverse ping;
preserve the result and correlate it with UART/counters.

After the baseline passes, address changes, stop/start, buffer accounting,
duplex payload integrity and a serial-only cold boot remain on the completion
checklist in ETHERNET-BRINGUP.md. This TFTP boot exercises adopted U-Boot state,
not standalone cold Ethernet initialization. No local IPv6 test applies to
this profile; subscriber IPv6 forwarding remains a later L2 optical test.
