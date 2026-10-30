# V2802RH L2 offload and hardware QoS audit

> Historical audit. For the current +201 implementation, verified status and
> outstanding QoS findings, see [the 2026-10-30 audit](HARDWARE-QOS-AUDIT-2026-10-30.md).

Audited on 2026-10-26 at `def58e73759a73d9a19a08f6c7e0208a42268da0`
(`vsol-v2802rh`, tag `v2802rh-boot-36s`). No production source, runtime
configuration or hardware was changed. This report covers the current
offload programmer, service planner, bridge integration, VLAN owners,
resource mapping and QoS interface. Hardware forwarding remains unverified.

**Result:** four L2 defects should be repaired before enabling offload.
Hardware QoS is still an accepted but unapplied request; implementing it
requires queue allocation and selection, not just writing weights/rates.

## Confirmed L2 findings

### F1 — P1: a seventeenth distinct service group corrupts the stack

`luna_xpon.c:657–724`, especially `710–712`; `luna_gpon_nic.h:26`;
`rtl9601d_l2.c:303`.

The backend holds 64 services, but the planner's priority array and the
worker's output array each hold 16 groups. A new VID executes
`services[n++] = d` and `priorities[j] |= ...` without checking capacity.
Seventeen valid, distinct, tagged VIDs on one supported VEIP are enough;
they need not use different GEMs. AddressSanitizer reproduced the first
out-of-bounds write in the actual extracted planner function.

The current two-VLAN deployment collapses into two groups and does not
trigger this case. Reject excess offload groups before either array write;
retain the working 64-service backend capacity and CPU forwarding.

### F2 — P1: bridge policy changes can leave hardware forwarding active

`rtl9601d_l2.c:357–368, 466`.

The driver registers only the atomic switchdev notifier. Linux 6.18 sends
STP state, bridge VLAN-filtering and multicast-snooping attributes through
the blocking notifier chain. The callback therefore misses these events.
The netdevice notifier watches `eth0`/`pon0`, not the bridge master, and
does not provide equivalent coverage for these attribute changes.

An enable-time `bridge_ready()` check does not protect a later policy
change. Hardware can keep forwarding after software blocks a bridge port
or enables a policy the driver explicitly cannot offload.

Some bridge-port flag changes have an atomic pre-change notification and
are caught; the defect is not that every notification is missing.

Evidence: prepared kernel `net/switchdev/switchdev.c:131–163, 474–553`,
`net/bridge/br_stp.c:29–52`, `br_vlan.c:890–917`, and
`br_multicast.c:1983–1993`. Register for the required chains and make
withdrawal follow the affected attribute's transition semantics.

### F3 — P1: independent VLAN owners overwrite each other's state

`rtl9601d_l2.c:181–219, 286–298`; `rtl9602c_eth.c:1158–1189`.

OMCI service synchronization, offload and Linux VLAN-device callbacks
write the same hardware table without shared ownership/reference tracking.
The spinlock serializes writes; it does not preserve their owners.

Reproduced with the current programmer and WAN callback in a mocked table:

1. Offload installs VLAN 100 with LAN, PON and CPU membership.
2. A WAN VLAN-add callback rewrites it as PON+CPU only.
3. `active` remains true and RX still sets the forwarding mark, so the
   bridge suppresses the missing software LAN forwarding path.
4. The VLAN-delete callback clears the entry entirely, also with
   `active=1`.

This can arise when creating/removing a `pon0.<vid>` device for a service
VID. It also affects the CPU path with offload disabled: the synchronization
bitmap remembers the VID as installed and does not repair an entry removed
by another owner. Conversely, service deletion can erase an entry still
needed by a VLAN device. Rollback restores an old snapshot rather than
reconciling owners that changed while offload was active.

Use one VLAN owner that derives membership from current consumers. A
callback changing an active binding must withdraw/reconcile forwarding
marks and hardware together. Propagate synchronization failures instead
of allowing the worker's final `error=0` to hide them.

### F4 — P1: a failed withdrawal is not a working CPU fallback

`rtl9601d_l2.c:55–92, 95–109, 309–316, 429–431`.

`clear_locked()` clears `active` before invalidating the classifier and
restoring VLANs. On persistent table failure, rules and LAN/PON membership
remain installed while CPU copies lose their forwarding marks. The worker
then removes the bridge hardware domain and stops at `saved=true`.
Hardware and software may both forward the same traffic; changed isolation
policy is also not assured. A sysfs disable write still returns its byte
count, though status can expose an error.

Fault injection reproduced live classifier entries and service VLANs with
`active=0, saved=1`. Retaining ownership is useful for retry, but does not
make withdrawal complete. Define a verified way to quiesce the data path
when table invalidation fails, then restore CPU forwarding only when the
hardware state permits it. Keep OMCC access separate from that operation.

### F5 — P2: table withdrawal runs synchronously in the OMCI path

`luna_xpon.c:156–176, 624–634`; `rtl9601d_l2.c:65–86, 103–108`;
`rtl9602c_eth.c:277, 612–615`.

A changed service calls `rtl9601d_l2_changed()` before the backend RPC
returns. With offload installed, it performs 64 classifier invalidations
and VLAN restores under `spin_lock_irqsave`. Each permanently busy table
operation can consume roughly 1 ms of polling. The two-service fault model
made 67 failed table attempts; the maximum 16-service case makes 81.
These are calculated polling bounds, not board timing measurements.

That can stall interrupts and OMCI replies for tens of milliseconds during
an offload failure, despite the tested daemon checkpoint improvement.
Use a short, verified withdrawal gate and move bulk table work out of the
OMCI response path. Do not restore synchronous checkpoint writes.

### F6 — P2: shutdown does not fence external work producers

`rtl9601d_l2.c:95–109, 435–451`; Ethernet probe registers the L2 cleanup
after the OMCI release action (`rtl9602c_eth.c:3055, 3102`).

Cleanup cancels L2 work before clearing the published `ep` pointer. The
OMCI backend remains attached until its later device-managed cleanup.
A concurrent backend change can read a non-NULL pointer and schedule work
after `cancel_work_sync()` has returned. That work can run after `ep` has
been cleared or released. This is a source-level race finding, not a
reproduced crash, and concerns driver unbind/reprobe rather than the
reported normal boot. Fence producers before draining the worker and
reinitialize the singleton state on a new attachment.

## QoS findings and verified resource map

### Q1 — the backend currently applies no requested QoS

`luna_xpon.c:492–507` returns success for T-CONT QoS and GEM rate requests,
logging the omission once. It does not retain those requests in backend
state. `luna_xpon_service` has no queue member, and service replacement
drops `omci_service_config.queue` (`luna_xpon.c:59–74, 608–623`). CPU TX and
the classifier both select a GEM flow that has one fixed SID-to-QID map.

This is a known implementation gap, not a reason to reject requests that
the tested Nokia provisioning depends on accepting. Add requested/applied
state and replay/cleanup behavior without changing that compatibility.

### Q2 — logical queues, physical queues and p-bit services differ

Current bindings (`luna_gpon.c:7032–7054, 7075–7139`):

| Resource | Current mapping | Consequence |
|---|---|---|
| OMCI T-CONT `0x8000 + slot` | Hardware T-CONT `8 + slot` | Eight backend data T-CONTs |
| Data T-CONT queue | Physical QID `32 + slot` | One queue per T-CONT today |
| Backend GEM slot | Flow/SID `4 + slot`, up to 19 | Sixteen backend GEM bindings |
| GEM upstream | SID -> T-CONT's QID | GEMs on one T-CONT share its queue |
| GEM downstream | CAM lookup, traffic/AES configuration, PON ingress | Separate from upstream scheduling |
| OMCC | Flow 64, T-CONT 16, physical QID 64 | Reserve throughout allocation/replay |

The Realtek GPON mapping is
`physical_qid = 32 * (hardware_tcont / 8) + logical_qid`, with
`logical_qid < 32`. Hardware T-CONTs 0–7 share QIDs 0–31; T-CONTs 8–15
share QIDs 32–63. These are shared banks, not 32 exclusive queues for
each T-CONT. The reference capacity table has 17 schedulers, 65 physical
queues and 65 classify SIDs.

Consequently, simply extending each current T-CONT to eight exclusive
queues requires 64 queues in a bank containing 32. An allocator must either
distribute hardware T-CONT bindings across banks or allocate only the
configured queues within explicit limits, while preserving logical ME IDs.

Also, eight p-bit mapper entries pointing to one GEM do **not** by
themselves require eight physical queues or SID aliases. The resolver can
select the GEM's upstream priority-queue pointer when QoS is configured
(`agent.c:3177–3193, 3390–3397`). Stock's two GEMs point at PQs `0x8007`
and `0x800f`; both mappers direct all p-bits to their respective GEM.
Allocate from the resolved T-CONT/PQ/GEM relationship, rather than from the
number of p-bit service records. A service queue change must then reach
both the CPU and hardware forwarding paths.

### Q3 — register layouts are established; rate semantics are incomplete

Offsets below are relative to PON-IP unless marked switch. They were
checked against the reference's register descriptors and SDK functions,
not inferred solely from current driver comments.

| Control | Offset and encoding | Established limit/remaining gap |
|---|---|---|
| SID -> QID | `0x20f8`, 7 bits, four entries/word | 65 SID mappings; reserve OMCC SID 64 |
| T-CONT queue membership | `0x23a0`, one 32-bit mask/T-CONT | Shared queue-bank ownership required |
| T-CONT enable | `0x23e4`, one bit/T-CONT | T-CONT indices 0–16 |
| Queue CIR / PIR | `0x2198` / `0x229c`, one word/QID, low 18 bits | Raw maximum `0x3ffff`; SDK setters pass values without conversion |
| Queue type | `0x23e8`, one bit/QID | SDK queue type 1 writes raw 0; type 0 writes raw 1 |
| Queue weight | `0x23f8`, 10 bits, three entries/word | SDK accepts 1–1023 and rejects zero |
| Global scheduler mode | `0x2194`, bit 19 | A global setting, not a per-T-CONT policy bit |
| Scheduler control/burst | `0x2194`, bits 18/17/16 and low 16 bits | Current word `0x66000`; no verified independent per-GEM burst control here |
| Token bucket timing | `0x2190`, 8-bit fields at 0 and 8 | Stock/current `0x956e`; absolute rate quantum still to establish |
| LAN priority -> egress queue | Switch `0x1c0b0`, 3-bit entries; map selector `0x1c0c0` | Eight LAN egress queues; separate from PON queue banks |
| Shared meters | Switch `0x25004`, 64-bit stride | 16 meters; current storm setup uses 8–15, so ownership matters |
| Port egress limiter | Switch `0x2d804`, 32-bit stride/port, rate bits 1–19 | Shared port scope; SDK shifts input right by 3 |

The backend API supplies bytes/s and bytes (`device.h:260`). Copying these
values into PON's 18-bit fields, or reusing the port-limiter conversion,
would be unsupported. Exact PON rate units, rounding, unlimited/disabled
values, burst units, strict-priority ordering and global WRR/WFQ mode
semantics still require authoritative evidence or controlled measurement.
An accepted OMCI zero weight also needs an explicit mapping; the SDK's
weighted-queue setter rejects zero.

### Q4 — current interfaces cannot express all hardware QoS requirements

`agent.c:2839–2871` rejects nonzero CIR/CBS and colour/meter attributes
before calling the backend. `set_gem_rate` carries only peak rate/burst.
`luna_xpon_set_uni_attrs` rejects non-default upstream/downstream UNI rates
(`luna_xpon.c:530–538`).

A queue shaper limits every GEM sharing that queue; a LAN port limiter
limits every service on that port. Neither establishes independent
downstream per-GEM policing. Full CIR/colour metering and downstream GEM
limits need verified mechanisms and, where necessary, additional request
state. Do not label a shared limiter as per-GEM QoS or change global burst
settings in response to one GEM without accounting for other consumers.

## Forwarding coverage that remains unproven

- RX marks use ingress port and in-frame VID, with no RX reason or
  destination disposition (`rtl9601d_l2.c:372–400`). The host checks prove
  matching and untagging; they do not prove that every marked packet was
  forwarded by hardware rather than trapped exclusively to the CPU.
- The driver does not consume static/local FDB updates or synchronize
  Linux FDB flushes with the independent hardware learner. No FDB replay
  callbacks are supplied when bridge ports are offloaded. Static FDB/MAC
  moves, local/control frames and bridges with additional ports need
  explicit support or a restricted eligibility rule.
- Multicast GEMs, snooping/MDB, translation/stacking, split-GEM p-bit
  mappings and partial priority coverage are deliberately refused by the
  planner. One unsupported service currently prevents the whole set from
  being offloaded. Preserve that CPU fallback until mixed operation is
  designed and verified.
- Table completion is polled, but installed bindings are not read back
  and compared. Status exposes only requested/active/count/error; it cannot
  establish CPU bypass, rule hit counts, per-service mapping or actual QoS.
- The stock dump establishes working optics and CPU/WAN service behavior,
  not a proven direct LAN-to-PON bridge. Its own classifier uses the CPU
  WAN interface. A/B/A hardware traffic tests remain necessary.

## Evidence and checks performed

Local reference: XZ000-G6 GPL's RTL9602C `vmlinux`, SHA-256
`34a1ef23a866b74a18b7fed2a9005ac7995f9f3a49cd5c55caf79c614135e5cc`.
Its storage directory contains `XZ000-G7`; the inspected artifact and
symbols are Realtek. It is a same-family reference, not the exact V2802RH
stock executable. Exact-board corroboration used
`~/Desktop/register-stock-online.log`, `~/Desktop/omcicli.log` and the
existing stock-reference document. No private OMCI payloads or keys are
copied into this report.

Key binary locations:

- `_ponmac_physicalQueueId_get`: `0x8013c2a4`;
  `dal_rtl9602c_ponmac_flow2Queue_set`: `0x8013c450`.
- Weight/type/CIR/PIR setters: `0x8013c5a8`, `0x8013c610`,
  `0x8013c674`, `0x8013c6d4`; queue add: `0x8013d890`.
- Global scheduling type: `0x8013cdc4`; meter setter: `0x801283ec`;
  port egress rate setter: `0x80128924`.
- Register list: `0x802b3ed0`, 24-byte descriptors; capacity table:
  `0x802b3c50`. Capacity offsets `0x4c/0x13c/0x148/0x17c` give
  `65/17/65/262143` for queue/scheduler/SID/rate limits.

REA's MCP provider refused this older MIPS ISA. The existing installed
Ghidra project and native MIPS disassembly were used directly; those are
local tool observations, not REA Evidence records. No tool installation
or reference artifact modification was needed.

Retained local evidence is under ignored `tmp/v2802rh/l2-audit/`:

| Check | Result / artifact |
|---|---|
| Actual planner, 17 distinct groups, ASan/UBSan | Reproduced overflow; `planner-overflow-audit.c`, `.log` |
| Actual programmer and WAN callback with mocked tables | Current IVL/matches/normal restore passed; owner clobber and persistent withdrawal failure reproduced; `ownership-audit.c`, `.log` |
| Ethernet/backend component cross-compilation | `rtl9602c_eth` and `luna_xpon` passed; `component-audit-build.log` |
| Existing omcid suite | MIB 2218, recovery 1537, transport 33, ownership 25, lifecycle 118 and proxy 16 checks passed |
| Backend-client test | Failed at `backend-client-test.c:159`: TX timing comment now precedes the expected `# ms` line; later assertions were not reached |
| Remaining test groups run independently | Seed-service 25 and OLT-watch 13 checks passed; `remaining-audit-tests.log` |
| Binary resource/layout audit | `qos-map.c`, `qos-map.disasm`, `qos-registers.c`, `qos-register-map.json` |

The table model does not reproduce switch timing, forwarding decisions or
OLT congestion. Successful compilation and host checks do not constitute
hardware-offload validation. The full omcid suite is **not** green because
of the trace-format test failure.

## Recommended implementation order

1. Repair F1–F4 with local regressions while offload remains disabled;
   address F5/F6 in the withdrawal/lifetime design. Consolidate VLAN
   ownership instead of adding another writer.
2. Add binding/readback/counter diagnostics and define bridge eligibility,
   CPU-trap handling and the failure fallback. Preserve the tested four-UNI
   emulation, Nokia seeds/capabilities, AES, IVL, SID preservation and
   deferred checkpoints.
3. Run the planned user-controlled RAM-boot off/on/off forwarding tests,
   including policy changes, VLAN-device creation/deletion and rollback.
4. Allocate queues from resolved configured PQs, respecting shared banks
   and OMCC reservations; retain requested/applied QoS separately.
5. Establish rate/priority/burst semantics, then implement scheduling,
   upstream shaping and downstream policing as separate measured steps.

This refines the [earlier plan](L2-QOS-PLAN.md): configured GEM/PQ bindings
come before any assumption of eight independent physical p-bit queues.
