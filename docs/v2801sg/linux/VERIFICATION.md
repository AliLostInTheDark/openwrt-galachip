# V2801SG verification — 2026-10-21

> Historical verification below predates the latest driver. See the latest
> checkpoint in [ETHERNET-BRINGUP.md](ETHERNET-BRINGUP.md) for image970e43bff23f,
> MAC-selective IPv4/IPv6 delivery and the unresolved IPv6 neighbor-recovery issue.

## Scope and provenance

Live UART and bounded management-LAN traffic tests; no reboot, driver reload,
flash write, PON mode change or optical-control write. A temporary TCP helper
was copied to /tmp and exited after its test. The root is a RAM-backed squashfs
loop plus tmpfs overlay; kernel.panic reads0. The user's supplied boot log
reports br-lan forwarding at26.54s; this run did not perform a physical cold
boot or reproduce that timing.

The current host TFTP artifact /srv/tftp/v2801sg-fast.uImage matches the
bin/targets/zte/zx279125 initramfs-uImage:3975769 bytes, SHA256
`a9ed80abb5548742deadca51d28b43b91feb096e8dc881b91589169029c155f0`.
This differs from the older size in XPON-INTEGRATION.md. The running LAN
module matches the staged squashfs module, SHA256
`0d314c83ee889c7bc1de7e8e22af26fe2903de97f8dec4960c980555b9fe866f`.
The whole running kernel was not independently hashed.

## Verified live

| Check | Result |
|---|---|
| Initial uptime |1841.23s; console responsive |
| Full-MTU ICMP,1000 packets at~500pps |1000/1000;0.480/0.551/0.757ms min/avg/max |
| Full-MTU ICMP,5000 packets at~500pps |5000/5000 in9.999s;0.479/0.546/0.832ms |
| Shared IRQ during1000 probes |IRQ17 increased1663->2663, exactly1000 |
| Shared IRQ around5000 probes |3063->8064; interval also includes background traffic |
| Duplex management TCP |1048576 bytes each way, both payloads checked,0.532s |
| Real service |SSH-2.0-dropbear banner received |
| LAN counters after tests |RX/TX errors and drops remain0; no new dmesg faults |
| Memory |MemTotal11032KiB; MemAvailable1688KiB initially,1604KiB after duplex |
| PON state |software stateO1, pon0 down, RX/TX counters0 |
| Tag partition reads |mtd4 is tagparam,64KiB; two reads have identical SHA256 |

Tag partition SHA256:
`e94a6b4d895726c0f7ef7d83836bb436e6b9461dd90c81dead7945b4020bd8b4`.
Read stability does not validate record checksums or prior erase/write behavior.
PC-side TCP payload SHA256:
`bf63d8a95fcc2e64619813aae35fdcbe871fdd9264caa3f365eb3aed0f679129`.
The board independently verified the received zero-filled payload.

The user's additional1500 small-packet pings also reported no loss, averaging
0.430/0.449/0.501ms at2/20/200ms intervals. Those are user-provided results;
the full-MTU and duplex results above were independently run here.

The IRQ counter tracks traffic, confirming hardware interrupt activity. The
250us fallback remains enabled: timer IRQs increased51957 over12.67s of an
idle observation (~4101/s, including the system tick). CPU counters indicate
~0.7% non-idle time over that sample, including console commands. Sub-ms RTT
does not prove that the fallback is unnecessary or that subscriber traffic
is hardware-offloaded. The5000-packet run exceeds the4096-entry RX ring size.

## Source findings that block a complete xPON claim

These are checks of current source, not additional optical hardware tests.

1. **GPON receive-register base is inconsistent.** zx279125-gpon.c:51..57
   writes RX configuration/FEC through base_gpon. The header describes those
   offsets under0x920c4000, while base_gpon maps0x920c0000. RX_CFG and
   INT_STATUS both have offset0, so the same mapped address has conflicting
   uses. Establish the actual layout from vendor instructions before fixing
   the mappings or running further optical initialization.
2. **OMCI provisioning/receive path is incomplete.** zx279125_omci_ops has no
   T-CONT or GEM provisioning callbacks. The GPON ISR acknowledges PLOAM
   status but has no OMCI receive delivery. The present pon0 transmit callback
   drops every skb. This does not exclude a future direct hardware bridge,
   but no working GEM/T-CONT/VLAN subscriber path is established here.
3. **EPON/auto support is overstated.** EPON set_mode returns success without
   configuring the EPON MAC. No platform OAM registration/LLID operations are
   present. pon-mode accepts gpon/epon only; auto falls back to gpon rather
   than scanning framing. Probe initializes GPON unconditionally, including
   when its reported initial mode is EPON. The live mode_current reads gpon.
4. **The documented optical-control contract is not implemented in full.**
   The platform xpon ops lacks set_tx_hold. Initial GPON setup does not
   implement the stated BOSA soft-disable sequence or10ms sync-loss shutoff.
   set_operational directly writes the TX enable bit. Burst/grant gating and
   rogue/O7 behavior are not demonstrated by registration or these LAN tests.
5. **Tag checksum enforcement is absent.** The parser validates magic,
   bounds and active flags, but never uses hdr->csum. It also substitutes
   MONU/derived serial values when identity records are missing, contrary to
   the documentation's zero-default description. Live identity logging is
   therefore not proof of checksum-validated factory identity ingestion.
6. **Shared IRQ ownership needs further review before optical traffic.**
   LAN zx_isr accepts top-level bits1 and2, while its NAPI path handles only
   queue0 and faults on other nonempty queues. Verify bit ownership and
   OMCI/OAM queue separation before concurrent PON traffic. No such concurrency
   was exercised while pon0 was down.

## Next verification gates

Correct the source/documentation discrepancies and validate register accesses
against the exact vendor binaries. Then verify management regression, shared
IRQ/queue ownership, actual OMCI RX/TX and provisioning callbacks. Optical
registration and end-to-end LAN-to-GPON offload remain unverified: require
VLAN/GEM/T-CONT counters, payload integrity and CPU-load evidence, separately
from management ping. No firmware installation or flash-write test is part
of these gates.

Host traffic logs: /tmp/v2801sg-verification-20261021/.
