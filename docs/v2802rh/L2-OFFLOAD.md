# V2802RH PON/LAN L2 offload

The RTL9601D SFU path in `rtl9601d_l2.c` programs upstream classification
and switch VLAN forwarding from omcid's installed services. It is specific
to `vsol,v2802rh`; the other Realtek boards keep their existing paths.
It starts disabled. Hardware forwarding has not yet been verified on the board.

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
- The Linux bridge ports share a switchdev hardware domain. Only matching
  service copies receive `offload_fwd_mark`, preventing duplicate software
  forwarding while preserving local delivery and CPU forwarding of other traffic.

Eight p-bit mappings to the same GEM collapse into one binding. Split-GEM
priority policies, partial p-bit coverage, VLAN translation/stacking, multicast
GEM services and conflicting bindings retain the CPU path. There can be only
one untagged service on each physical LAN port. The configured ONU identity
and advertised UNI count are preserved.

The backend advertises `OMCI_TOPOLOGY_F_FULL_UNI_ENTITY`, so omcid preserves
full UNI IDs for service and administrative operations. Profile truncation
would otherwise turn both VEIP 0x0601 and PPTP 0x0101 into 1.

## Enable after rebuilding

The existing `br-lan` must contain both `eth0` and `pon0`, with both ports
forwarding, bridge VLAN filtering off and multicast snooping off. This first
path does not program the bridge MDB. The L34 NAT engine and per-port VLAN
mode must also be off.

```sh
echo 0 > /sys/class/net/br-lan/bridge/multicast_snooping
echo 1 > /sys/class/net/pon0/device/l2_offload
cat /sys/class/net/pon0/device/l2_offload
```

`requested=1 active=1 services=N error=0` means the tables were programmed;
it does not prove that traffic crossed the hardware. Capture port MIBs and
CPU load during traffic in both directions to establish that.

Disable and restore the previous VLANs, LAN PVIDs and flood settings:

```sh
echo 0 > /sys/class/net/pon0/device/l2_offload
```

Service changes, channel loss, bridge attribute changes and NIC recovery
withdraw the rules before rebuilding them. Classifier slots 32–95 are used
only if they are invalid and lie in pattern 0. Table access is serialized
with NIC VLAN updates. Timed-out rollback retains ownership and reports an
error instead of declaring a successful restore.

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
suite passed, including full-UNI regression coverage. Full image rebuild and
board traffic validation remain pending.
