# V2802RH PON/LAN L2 offload

The RTL9601D SFU path in `rtl9601d_l2.c` programs upstream classification
and switch VLAN forwarding from omcid's installed services. It is specific
to `vsol,v2802rh`; the other Realtek boards keep their existing paths.
It is requested by default on V2802RH and activates when the bridge and
installed services pass the checks below. Fresh images disable bridge
multicast snooping in the network defaults. The 2026-10-26 off/on/off capture
demonstrates LAN2 CPU bypass in both directions; remaining checks are
recorded below.

## Forwarding

For each supported bidirectional service:

- LAN ingress is classified by physical port and network VID, or by an
  untagged match. The classifier assigns the GEM's installed upstream SID.
  These rules do not restrict EtherType, so they cover ARP, IPv4, IPv6 and
  PPPoE Ethernet frames.
- The VLAN includes the associated LAN port(s), PON and CPU. A VEIP at
  entity 0x0601 binds both physical LANs; physical UNI bindings use the DT
  port map. Unknown or emulated UNI ports are not guessed.
- An untagged service sets the LAN PVID and strips the tag toward the LAN.
  It also accepts frames already carrying the network VID, as the CPU path
  does. CPU copies retain the VID until RX classification; LAN copies are
  then untagged in software so management traffic remains usable.
- Downstream service VLANs reach the LAN through the switch's L2 lookup and
  VLAN membership. Learned unicast forwarding bypasses the CPU.
- The Linux bridge ports share a switchdev hardware domain. Matching service
  copies with normal RX reason 0 receive `offload_fwd_mark`; trapped copies
  remain available to software forwarding. Counters expose both paths and
  the last observed reason for board validation.

Eight p-bit mappings to the same GEM collapse into one binding. Split-GEM
priority policies, partial p-bit coverage, VLAN translation/stacking, multicast
GEM services and conflicting bindings retain the CPU path. There can be only
one untagged service on each physical LAN port. The configured ONU identity
and advertised UNI count are preserved.

The backend advertises `OMCI_TOPOLOGY_F_FULL_UNI_ENTITY`, so omcid preserves
full UNI IDs for service and administrative operations. Profile truncation
would otherwise turn both VEIP 0x0601 and PPTP 0x0101 into 1.

## Default enablement and runtime control

No enable command is needed on a fresh image. After O5 and provisioning,
check the state:

```sh
cat /sys/class/net/pon0/device/l2_offload
```

An upgrade that preserves `/etc/config/network` also preserves its bridge
settings. If snooping is enabled there, offload remains inactive until that
setting is changed. `requested=1` alone does not mean forwarding is active.

The existing `br-lan` must contain only `eth0` and `pon0`, with both ports
forwarding, bridge VLAN filtering off and multicast snooping off. This first
path does not program the bridge MDB. The L34 NAT engine and per-port VLAN
mode must also be off. Non-local user FDB entries and enabled isolation-leak
settings prevent offload; their forwarding policies are not implemented.
To enable it again after a runtime disable, with a compatible bridge:

```sh
echo 0 > /sys/class/net/br-lan/bridge/multicast_snooping
echo 1 > /sys/class/net/pon0/device/l2_offload
cat /sys/class/net/pon0/device/l2_offload
```

`requested=1 active=1 services=N error=0` means the tables were programmed;
it does not prove that traffic crossed the hardware. Capture port MIBs and
CPU load during traffic in both directions to establish that.

Disable until reboot and restore the previous VLANs, LAN PVIDs and flood
settings:

```sh
echo 0 > /sys/class/net/pon0/device/l2_offload
```

Service changes, channel loss, bridge attribute changes and NIC recovery
first isolate direct LAN/PON egress with the two packed port-isolation
words. CPU and OMCC destinations remain permitted. Bulk table updates run
in work context, with readback, instead of polling every rule in an OMCI
callback. Classifier slots 32–95 are used only if invalid and in pattern 0;
only rules actually written are invalidated on withdrawal.

OMCI and VLAN devices share service-VID ownership. Removing one consumer
does not remove a VLAN still needed by the other. Restore derives those
entries from current consumers, and flushes dynamic LAN MAC entries before
releasing isolation so old IVL entries cannot strand downstream replies.

A failed restore keeps ownership and isolation, exposes `gated=1` and an
error, and retries after 100 ms. Data can be interrupted while gated;
`active=0` alone does not promise a completed CPU fallback. A sysfs write
waits for reconciliation and returns its error. Successful disable has
`requested=0 active=0 services=0 error=0 gated=0`.

## Capture an off/on/off comparison

After each phase, run the same traffic test from a LAN client to an existing
internet peer, and capture these again before and after that traffic:

```sh
omci-diag
ethtool -S eth0
cat /proc/net/dev
cat /proc/stat
cat /sys/class/net/pon0/device/l2_offload
cat /sys/class/net/pon0/device/l2_offload_bindings
cat /sys/class/net/pon0/device/l2_offload_counters
```

`omci-diag` now includes the three L2 attributes. Bindings show VID, SID,
physical LAN mask, untagging and VLAN-table readback. The counters count CPU
copies marked as already forwarded or kept for software; they are not
hardware rule-hit counters. Correlate LAN/PON/CPU MIB deltas, GEM counters,
CPU load and throughput to prove bypass in each direction. Check each LAN,
then both together, and confirm disabling restores working traffic. Do not
send arbitrary test traffic into the ISP's management service.

## Evidence and checks

The classifier layout was inspected offline in Realtek's GPL vmlinux,
SHA-256 `34a1ef23a866b74a18b7fed2a9005ac7995f9f3a49cd5c55caf79c614135e5cc`:
`rtl9602c_raw_cf_rule_set`, `rtl9602c_raw_cf_usAct_set`,
`RTL9602C_CF_RULE_48_P0_FIELDS`, `RTL9602C_CF_MASK_48_P0_FIELDS` and
`RTL9602C_CF_ACTION_US_FIELDS`. The V2802RH stock register dump confirms
CF_CFG 0x0001d009 and the PON classifier selection. This establishes the
register format, not successful hardware execution.

Changed Ethernet/backend objects cross-compiled. Sanitized host checks of the
actual programmer and planner passed for match isolation, SID selection,
eight-pbit merging, UNI binding, RX copy marking/untagging, VLAN/PVID restore,
occupied slots, stale generations and injected write failures. The omcid
suite passed, including full-UNI regression coverage. Board traffic
validation beyond the LAN2 comparison below remains pending.

## Board result, 2026-10-26

The user's `/home/ali/Desktop/offload-l2.log` has SHA-256
`8ee2286e0c67ad7e50931447511d5771e747cc2c54b1c00f9eabf0a4157054ac`.
Its six snapshots cover off/on/off on the same boot. DT maps physical
port 0 to LAN2, port 1 to LAN1; port 2 is PON and port 3 is CPU.

| Counter delta | Off | On | Off again |
|---|---:|---:|---:|
| LAN2 RX bytes (upstream) | 146,492,988 | 372,495,853 | 108,497,321 |
| LAN2 TX bytes (downstream) | 5,716,167 | 595,223,244 | 5,707,780 |
| CPU-port RX bytes | 152,270,461 | 0 | 112,578,237 |
| CPU-port TX bytes | 151,760,635 | 0 | 112,217,005 |
| `p2_tx_bridge_discards` | 68,159 | 0 | 49,049 |
| LAN2 RX drop events | 867 | 64 | 1,749 |
| Average CPU busy | 23.8% | 11.9% | 21.0% |

Offload-on has two bindings, VID 100/SID 4 (untagged) and VID 660/SID 5
(tagged), both with LAN mask 0x3 and successful VLAN readback. It stays at
`active=1 error=0 gated=0 retries=0`. During that interval all eth0/pon0
Linux packet/byte counters also stay unchanged, as do RX marking counters
and softirq CPU time. Paired LAN2/PON byte and unicast counters rise in
both directions. This demonstrates bulk hardware forwarding without CPU
copies for this LAN2 traffic, beyond the installation status alone.

The user reports roughly 2 Mbps down/64 Mbps up with offload off, and
plan speed with it on (300 Mbps service). These snapshots cover changing
speed-test traffic plus diagnostics; their byte totals are not sustained
throughput measurements. Disabled forwarding resumes but performs poorly
and has substantial PON bridge-discard counter increments. Average CPU
load does not establish saturation; the fallback failure needs diagnosis.

LAN1 counters are zero throughout this capture. The later user test below
covers traffic on each LAN; simultaneous-port behavior remains untested.
Management/OMCI under load, MAC moves, service replacement,
re-ranging and higher-rate capacity also remain pending. This result does
not complete the full validation gate. The user subsequently requested
default enablement for the V2802RH branch with these checks still open;
other boards retain their existing behavior.

The 2026-10-26 repair additionally passed 196,608 TCAM comparisons, failures
at all 109 install and 18 withdrawal table-transaction positions, readback
corruption, persistent table/flush failure and retry, port-isolation/CPU
preservation, changing VLAN owners, stale generations, static-FDB refusal,
bridge notifications and the producer shutdown fence. Planner checks cover
1–64 distinct groups and the 18-record Nokia mapping. These host checks use
the actual functions with mocked hardware and do not prove board execution.

The withdrawal controls were checked in the same Realtek binary:
`dal_rtl9602c_port_isolationEntry_set` at `0x8011bc5c` uses register index
`0x275` (`0x27000`, 11-bit packed entries); dynamic port flushing uses
`rtl9602c_raw_l2_flushCtrl_set` at `0x8012ffa4` and
`rtl9602c_raw_l2_flushEn_set` at `0x80132030` (registers `0x17024/0x17028`).
The exact-board stock capture has isolation leaks disabled.

Hardware QoS remains unapplied. It follows the L2 hardware validation gate
in [L2-QOS-PLAN.md](L2-QOS-PLAN.md); this repair does not change the tested
advertised capabilities, identity, optical calibration or queue mapping.

## Default boot and speed variation audit, 2026-10-26

The subsequent boot capture (attachment `7ab7e5ce-f2e1-462e-ae0e-cbe91ea594a1`)
has SHA-256 `b805e0343ccb8785858fcfe4baae4a89cf180c791451b5a06991b6be1019ad71`.
It boots revision `r37655+139-6db25ee835` and reports
`requested=1 active=1 services=2 error=0 gated=0 generation=366 retries=0`.
Default activation is therefore confirmed. The user reports successful
traffic tests on both LAN1 and LAN2, with initial 275–280 Mbps download
and burst/dip/recovery on upload, reaching the 300 Mbps plan rate. This
pattern repeats while online, beyond initial boot or a cable move.

Source audit at `c0bea668e5`:

- `rtl9601d_l2.c` classifies all EtherTypes and all p-bits for each accepted
  single-GEM binding. There is no TCP/session warm-up path, periodic traffic
  sampling or 275–300 Mbps rate setting. Normal learned unicast is switched
  directly; keeping CPU VLAN membership permits management, flooded and
  trapped frames without forcing a copy of every learned unicast frame.
- L2 table updates follow configuration/topology/recovery events. The
  100 ms retry is conditional on an incomplete/error state. All 12 install
  messages in this capture fall between 37.207619 and 37.842419 seconds,
  during provisioning. None later appears in the supplied log. The port-0
  link-down message at 92.014801 seconds is a separate link event.
- `luna_xpon_replace_service()` skips identical service replays; omci.agent
  also compares VLAN settings before writing them. Its normal configuration
  poll does not continually tear down unchanged offload bindings.
- `luna_xpon_hw_tcont_install()` selects maximum raw PIR `0x3ffff`, CIR 0
  and one queue per T-CONT. OMCI queue/rate callbacks remain unapplied.
  No plan-specific 275/280/300 Mbps cap is programmed by this path. This
  does not establish equivalence to stock's queue/buffer policy.
- Stock-derived unknown-unicast/broadcast/multicast/DHCP storm meters and
  Ethernet flow control remain enabled. Unknown destinations can be flooded
  to CPU until learned. Drop/pause deltas during the dip are needed before
  blaming or changing these controls.

No sustained CPU leak or rate-limit defect is established by this source
audit. The earlier LAN2 capture proved CPU bypass during its measured
interval; this new boot capture contains no traffic counters from a dip.
It cannot distinguish local drops/flow control from client, TCP, ISP/OLT
or test-server behavior. TCP startup and congestion recovery can also
change the sending rate ([RFC 5681, section 3.1](https://www.rfc-editor.org/rfc/rfc5681.html#section-3.1)).

For the next test, leave offload enabled and start this capture on the ONU,
then run a speed test from the LAN client. It samples about once a second
for one minute and changes no network settings:

```sh
(
    d=/sys/class/net/pon0/device
    i=0
    while [ "$i" -lt 60 ]; do
        echo "=== sample $i ==="
        cat /proc/uptime
        cat "$d/l2_offload" "$d/l2_offload_counters"
        ethtool -S eth0 | grep -E 'p[0-3]_(rx_octets|tx_octets|rx_drop_events|rx_crc_align_err|rx_symbol_err|rx_pause|tx_pause|tx_discards|tx_bridge_discards):'
        cat /proc/net/dev
        grep '^cpu ' /proc/stat
        i=$((i + 1))
        sleep 1
    done
) > /tmp/l2-speed.log 2>&1
```

Retrieve `/tmp/l2-speed.log` after completion. Stable generation/active state
and CPU-port traffic small relative to LAN/PON data would exclude bulk CPU
forwarding during that interval. Rising drop/pause counters would identify
a local loss/backpressure symptom requiring further diagnosis; quiet local
counters still would not identify which external endpoint or network caused
the variation. Use the same client, server and test settings for any stock
comparison. No forwarding-code change or new firmware is justified yet.
