# V2802RH hardware QoS audit — 2026-10-30

## Result and scope

Audited source: `32555d7610de3eb31050228cd85f6dbfa2d55223`.
Read-only hardware snapshot: OpenWrt `r37655+201-6db25ee835`.

Upstream queue allocation, strict/weighted selection and peak-rate writes
already exist. Full hardware QoS is **not complete**. The next implementation
must first repair queue ownership on partial failure and expose applied state;
then extend classification before attempting multi-class offload tests.

This audit changes documentation only. No ONU configuration, rates, MIB,
firmware, laser state or forwarding policy was changed. Existing port-rate,
management and physical-blackout cases remain closed. Findings below concern
additional QoS capabilities and exceptional update paths, not a newly observed
throughput regression.

## Evidence and reproducibility

Primary sources, relative to the repository root:

| Source | Audited responsibility |
|---|---|
| `package/utils/xpon/omcid/src/agent.c` | OMCI decoding, TD/PQ/TS resolution, service reconciliation, rollback |
| `package/utils/xpon/omcid/src/backend-client.c`, `backend.c` | Typed userspace/kernel requests |
| `target/linux/realtek-luna/files-6.18/net/xpon/omci/backend.c` | Kernel backend dispatch |
| `target/linux/realtek-luna/files-6.18/include/net/xpon/omci.h` | QoS contract: policy/8 weights, peak rate/burst |
| `target/linux/realtek-luna/files-6.18/drivers/net/ethernet/realtek/luna_xpon.c` | Requested state, queue grouping/allocation, service selection/replay |
| `.../luna_xpon_hw.h`, `.../luna_gpon.c` in the same directory | Queue/SID programming, packed registers, drains, locking |
| `.../rtl9601d_l2.c`, `.../rtl9602c_eth.c` | Hardware classifier, PCP override, UNI rate controls |

Retained exact-unit stock exports were re-read under
`/home/ali/openwrt-galachip/tmp/v2802rh/l2-audit/`:
`qos-stock-sort.c`, `qos-stock-rates.c`, `qos-deep-reference.c` and
`qos-register-map.json`. Their original extraction/provenance is documented
in [the earlier audit](HARDWARE-QOS-AUDIT.md). This audit does not claim a
fresh stock extraction or fresh binary decompilation. Same-family SDK evidence
is distinguished from exact-board behavior; the RTL8367B scheduling enum in
bundled headers is for a different switch and cannot settle the PON enum.

Local evidence (not publication files):

- `/tmp/v2802rh-qos-audit/make-harness.py`, `backend.c`, `backend`: extracts
  the current production queue allocator/programmer into simulated hardware.
- `/tmp/v2802rh-qos-omcid-tests.log`: complete existing daemon test suite.
- `/tmp/v2802rh-qos-live.txt`: read-only MIB and `/proc/gpon` snapshot; contains
  subscriber identity and must not be published unredacted.

The host harness exercises production C, not a rewritten allocation algorithm.
Hardware writes/locks are mocked; it proves software state transitions, not
physical arbitration or shaper calibration.

## Current pipeline and capability matrix

Upstream: OMCI T-CONT/PQ/TS and GEM TD -> daemon resolution -> typed backend ->
retained QoS/rate state -> GEM priority groups -> T-CONT queue mask ->
SID-to-QID map -> PON scheduler -> OLT grants.

LAN traffic selects a GEM/SID through the hardware CF classifier; CPU traffic
uses `luna_xpon_tx_flow()`. The queue is selected by SID, not independently
for every packet inside a single GEM. PCP marking alone does not create a
separate scheduling class.

| Feature | Current implementation | Evidence / boundary |
|---|---|---|
| Physical UNI ingress/egress caps | Implemented, with dedicated LAN2 HSGMII path | Previously hardware-tested; closed case |
| Upstream queue allocation | Dynamic grouping within QIDs 32–63 | Host-tested; partial-failure defect below |
| Strict priority | Smaller OMCI priority gets higher physical QID | Stock packing corroborates; contention not measured |
| Weighted scheduling | Per-queue type/weight programmed; zero weight clamped to 1 | Host-tested writes; WRR versus WFQ arbitration unverified |
| Upstream GEM peak rate | Rated GEM gets its own queue; PIR programmed | Stock-derived conversion; no new per-GEM rate measurement |
| Upstream CIR / guaranteed bandwidth | Not implemented | TD resolver rejects nonzero CIR; queue CIR always zero |
| Per-GEM PBS/CBS, colour policing | Not implemented | Burst ignored in Luna; richer TD modes rejected by resolver |
| Downstream per-GEM policing | Not implemented | Nonzero peak request accepted but not applied by Luna |
| Downstream per-UNI queue scheduler | Not implemented through these callbacks | Seeded downstream PQs do not imply working hardware policy |
| Hierarchical scheduling | Not supported by resolver | Nonzero scheduler parent / mixed roots rejected |
| Per-port upstream PCP override | Implemented in CF action | Marking, distinct from queue selection/guaranteed bandwidth |
| Same-VLAN PCP -> different GEM offload | Not supported by current L2 planner | Explicitly refuses multiple flows for one VID |
| Requested/applied QoS diagnostics | Incomplete | Logs and internal caches; no complete queryable per-GEM state |

OLT DBA remains authoritative for upstream grants. ONU scheduling or shaping
cannot guarantee bandwidth that the OLT does not grant.

## Findings

### F1 — High: partial SID failure leaves queues outside allocator ownership

`luna_xpon_qos_tcont()` programs queues, installs the union queue mask, and
moves GEM SIDs one at a time. It updates `t->pool` only after every SID move
succeeds. A failure returns without restoring previous SIDs, masks or policy.
`luna_xpon_pool_free()` consults `t->pool`, not the actual union mask/SIDs.

Reproduced with the extracted production function under ASan/UBSan:

1. Three GEMs on T-CONT 0 initially use QID 32.
2. Reconciliation allocates 40/41; first SID move succeeds to QID 40.
3. Inject failure on the second SID move. Hardware mask still includes 40,
   GEM 1 points to 40, but `tcont[0].pool` remains zero.
4. Reconcile another two-GEM T-CONT: it also allocates QID 40.

Both scheduler masks now claim one physical queue. This contradicts the
comment that failure simply leaves traffic on its old queue. Even an automatic
retry of the first T-CONT is not a transaction guarantee.

Required repair: reserve staged ownership before hardware mutation; snapshot
old masks, SID maps and queue parameters; commit or rollback coherently. If
rollback fails, quarantine every potentially referenced queue until readback
and recovery prove it free. No queue may become available merely because an
operation returned an error. Protect OMCC QID 64 throughout.

### F2 — High: accepted requests are not equivalent to applied QoS

`luna_xpon_qos_sync()` returns void and logs hardware failures. Both QoS and
rate callbacks then return success. Downstream GEM rates and upstream bursts
also return success without enforcement, with only `pr_info_once()` messages.
A full rate cache similarly logs once and leaves the new GEM unlimited.

Compatibility acceptance may be needed for current Nokia provisioning, but
neither an OMCI success response nor the MIB proves enforcement. Add separate
requested/applied/pending/unsupported/error status, scope and effective units.
Keep compatibility policy explicit; do not blindly change all such responses
to errors and risk disrupting working OLT provisioning.

### F3 — High: multi-class classification is missing from the offloaded path

`luna_xpon_l2_services()` groups by VID and rejects differing flows for the
same VID. It also requires the merged PCP bitmap to cover all eight PCPs.
`v2802_l2_rule()` matches port/tag/VID, not packet PCP; its PCP action is an
optional per-port rewrite. CPU service selection can distinguish PCP.

Consequently a typical one-VLAN/two-GEM priority setup cannot currently stay
on this L2 offload path. Enabling queue scheduling alone does not solve this.
Implement bounded PCP-aware CF rules and treatment order, resource ownership,
readback and rollback before claiming full hardware multi-class forwarding.
An unsupported configuration may fall back to CPU; that is not QoS offload.

### F4 — Medium: teardown and readback are incomplete

- `luna_xpon_hw_tcont_queues()` is void and does not verify the written mask.
- `luna_xpon_queue_free()` ignores drain and reset failures before releasing
  pool ownership. `luna_xpon_hw_tcont_remove()` similarly ignores cleanup
  failures and has no error return.
- Queue programming checks PIR/type/weight, but not the CIR-zero readback.
- SID commit writes before checking; a timeout does not establish that the
  old SID map survived unchanged.

Propagate failures internally, retain/quarantine ownership and verify masks,
SID validity/maps and rate/type/weight/CIR at the transaction boundary.
Never reset or reallocate a queue still referenced by a surviving consumer.

### F5 — Medium: weighted semantics and rate boundaries need proof

The backend supports one policy for a T-CONT, not arbitrary mixed strict/WRR
queues or multiple scheduler stages. It uses `wfq = n > 1 && policy != strict`;
a single group stays strict. Individually zero weights become one; all-zero
weighted configurations are rejected by the daemon. These are distinct cases.

The global scheduling selector at PON `0x2194` bit 19 stays at the stock
initialization value (`sch_ctrl=0x66000`, bit 19 clear in the live snapshot).
No per-T-CONT callback manages the global selector. Calling queue type 1
“WRR” does not prove packet-weighted WRR rather than byte-weighted WFQ.
Use unequal frame sizes under contention to establish arbitration.

`luna_xpon_pir()` maps nonzero bytes/s to `clamp(rate >> 10, 1, 0x3ffff)`;
zero maps to `0x3ffff` (the driver's unlimited convention). Thus:

- 1 B/s becomes raw 1: a minimum quantum, not a 1 B/s promise.
- 12,500,000 B/s becomes raw 12,207, not an exact 100,000,000-bit/s setting.
- 268,434,432 B/s and larger collapse to the same raw code as unlimited.

Exact-stock arithmetic corroborates the shift, not calibrated line throughput
or every sentinel. Expose quantization/saturation; verify with per-GEM tests.
Physical-UNI cap measurements must not be reused as proof of PON PIR units.
The shared burst-control word is not independent per-GEM burst support.

### F6 — Medium: advertised/logical resources exceed implemented controls

The topology advertises eight queues per T-CONT, eight schedulers, downstream
queues per emulated UNI and 256 GEM ports. The backend has only sixteen GEM
slots and eight T-CONT slots, and allocates physical queues on use. This can
be a deliberate compatibility model, but is not full resource enforcement.
Do not change the user's four-UNI emulation to match two physical jacks.

PQ size/discard/back-pressure fields are parsed/stored but the QoS backend
contract only carries enable/policy/eight weights. Those fields therefore have
no hardware application through this contract. Scheduler priority/weight is
also not represented as an additional scheduling stage.

Data bound to the special OMCC Alloc-ID uses the protected OMCC queue; QoS
sync skips it (`t->hw` is false). This protects control traffic but also means
such data is not independently shaped. Document/refuse that combination as
appropriate without rewriting OMCC policy to satisfy a data QoS request.

### F7 — Medium: synchronous reconciliation can delay control-plane work

QoS setters and service updates synchronously rescan T-CONTs and program
queues. Drains poll up to 10,000 iterations per stage while holding the FSM
spinlock. Multiple drains/remaps on one provisioning request can accumulate
latency; there is no dedicated deferred QoS worker or applied-generation
interface. Current single-GEM/default operation avoids much of this work.

Keep OMCI replies fast by separating validated desired state from bounded
hardware reconciliation, with generation checks, retry limits and explicit
failure status. Measure actual reply latency before changing retry timers.

## Lifecycle, capacity and live baseline

Eight fallback queues occupy 32–39; extra groups use 40–63. With sixteen
upstream GEM slots, at most sixteen groups are needed, so ordinary allocation
fits. The exhaustion branch must still not silently merge rated groups;
F1 demonstrates how exceptional ownership can invalidate ordinary bounds.

GEM-rate requests arrive before GEM installation and are retained by GEM ID.
T-CONT QoS is retained by logical slot. OMCC channel reinstall clears installed
T-CONT/GEM/queue/service caches via `luna_xpon_forget()` but retains requested
rate/policy for replay. Backend MIB reset clears those requests. Normal GEM
delete calls rate removal through the daemon before deleting the CTP. The
exceptional failures above require dedicated reset/delete/replay tests.

Read-only +201 snapshot:

- O5 and active offload: 3 bindings, error 0, gated 0; steering mode 2.
- LAN1 1G, LAN2 2.5G HSGMII; both ordinary UNI rate raw fields 524287;
  dedicated HSGMII field 163840 / `0xa8000` (unrestricted baseline).
- Eight T-CONTs, eight TS instances, 104 PQ instances (64 upstream and
  40 downstream for four emulated UNIs plus VEIP), all default origin.
- No class-280 traffic descriptors. T-CONT 0x8000/0x8001 use Alloc-ID
  256/257; remaining T-CONTs unallocated. Default TS policy is WRR.
- OMCC QID 64: PIR `0x3ffff`, CIR 0, type strict, weight 1, mask 1.

Default-origin MIB policy alone does not expose `qos_configured` or actual
per-data-queue hardware state. `/proc/gpon`'s scheduler line covers OMCC,
not all data queues; its legacy `data_owner` fields are not the userspace
backend's installed-service status. Do not interpret those zero legacy fields
as broken forwarding. No new queue-contention or rate test was run on hardware.

## Checks completed

Existing daemon suite passed with ASan/UBSan: 2,218 engine, 1,537 recovery,
33 transport, 25 ownership, 118 lifecycle, 16 proxy, 1,054 typed client,
25 seeded-service and 20 OLT-watch checks. These do not exercise the kernel
queue allocator's MMIO rollback, so their success does not invalidate F1.

Fresh production-function harness passed strict-order allocation, weighted
value retention, shared unshaped priority and isolated rated GEM cases.
It reproduced both the unowned-live-queue failure and subsequent double
ownership without sanitizer violations. Rate minimum/maximum were checked.

## Ordered implementation and validation plan

1. **Ownership and observability:** repair F1/F4; expose desired/applied
   generations, per-GEM SID/QID/T-CONT, requested/effective rate and weight,
   pending/error/unsupported flags and pool ownership. Capture current state
   without clear-on-read traffic-counter loss.
2. **Failure and lifecycle tests:** inject every drain/write/readback/SID
   failure, including rollback failure; assert unique ownership, protected
   OMCC, no stale references, bounded retries and no silent unlimited fallback.
   Cover delete, rate-before-create, failed create, rate changes, reset and
   retained-MIB channel replay. Preserve the proven default forwarding path.
3. **Classification:** add PCP-aware offload for distinct GEMs of one VID;
   validate marking/treatment order, CPU-path agreement, rule exhaustion,
   untagged ingress and both LANs. Report explicit fallback reasons.
4. **Scheduling and peak shaping:** prove global weighted-mode semantics,
   strict ordering, zero weights, quantization/sentinel behavior and effective
   limit scope before adding defaults or UI claims.
5. **Additional capabilities:** separately investigate downstream per-GEM
   meter resources and bursts/CIR; then downstream per-UNI scheduling, buffer
   and back-pressure attributes. Use board-specific reference evidence;
   per-port caps and global burst registers are not substitutes.

Hardware tests need two competing GEMs/queues **under the same T-CONT** on a
controlled peer/test service. The current two Internet services on different
T-CONTs cannot prove within-T-CONT scheduling. Run strict starvation/latency,
1:1 and 1:3 weighted tests with equal and unequal frame sizes, individual and
aggregate caps, burst tests only after burst support exists, and both LANs.
Correlate queue/GEM counters, CPU-port bytes/load, offload state and OMCI reply
latency; `active=1` alone is insufficient. Check other services stay unaffected.

After each hardware step, repeat reboot, 10-second and five-minute physical
fiber loss with retained and reset MIB behavior when available, checking
management, offload, rates and independent service isolation. Keep a bounded
rollback to the unrestricted baseline. No live disruption is part of this audit.


## Addendum: user-supplied XZ005-G6 WIND3 GPL tree

Inspected `/home/ali/Desktop/TP/XZ005-G6_WIND3_GPL/` directly after the
initial audit. Its particularly useful artifact is
`platform/targets/XZ005-G6V1/ITWIND3/kernel/vmlinux`:

- SHA-256 `7ad7b661efd9213eaf43c2100f2e8ea5ab5453f997b45aefa4035717a0c2a5e8`.
- Original unstripped big-endian MIPS ELF with local/global symbols; distinct
  from the previous reference ELF and the raw-derived synthetic carrier.
- DWARF exists, but the inspected debug information has one compilation unit,
  `drivers/net/re8686_rtl9602c.c`. Do not promise full PON SDK structure types
  merely because `file` reports `with debug_info`.
- `linux.7z` in that directory is actually a legacy LZMA uImage, not a 7z
  archive. No extraction or execution was needed for these observations.

The source search did not locate the proprietary PON DAL/OMCI implementation
in the supplied tree. Public BOA glue does exist:
`sdk/rtl9601/user/boa/src/LINUX/rtusr_rg_api.c:4947–4983` stages per-queue
strict/weighted type and weights through `RTK_TR142_IOCTL_SET_QOS_QUEUES`.
Its `RTK_RG_QoS_Queue_Set()` reverses user queue order into hardware indices
(`weights[7-i]` in the GPON branch). This is corroborating application glue,
not proof of the PON global weighted enum.

Fresh `readelf`/cross-`objdump` observations from the original ELF:

| Function | Address | Useful evidence |
|---|---|---|
| `dal_rtl9602c_ponmac_schedulingType_set` | `0x8012f6c8` | Accepts values 0/1 and writes their boolean value through register 0x3ab, field 0x77b; enum names still need matching PON evidence |
| `dal_rtl9602c_ponmac_queue_add` | `0x80130194` | Named calls expose queue drain, scheduler mask, type/weight, CIR/PIR and SID validity sequence |
| `dal_rtl9602c_rate_shareMeter_set` | `0x8011acb0` | Concrete meter programming implementation available for follow-up |
| `dal_rtl9602c_rate_shareMeterBucket_set` | `0x8011ac34` | Concrete bucket setter available; physical burst units still require tracing |
| `dal_rtl9602c_classify_field_add` | `0x801389a4` | Classifier field builder available for PCP-match investigation |
| `dal_rtl9602c_classify_cfgEntry_add` | `0x8013c2ec` | Classifier entry programming available |

The queue-type setter at `0x8012ef14` inverts its API argument before
writing register 0x3b0 / field 0x694. Consequently API enum values must not
be copied directly into raw register bits. The queue-add branch that forces
weight one passes API type 1, yielding raw type 0; its other branch applies
the supplied weight and API type 0, yielding raw type 1. This corroborates
the current strict/weighted raw-bit distinction without settling WRR/WFQ.

Retained symbol table, DWARF and focused assembly are in
`/tmp/v2802rh-qos-audit/gpl-*`. These are static shell-tool observations.
REA successfully opened/identified the ELF, but its native analysis provider
refused this MIPS ISA (requires MIPS32R2); no flags were rewritten to bypass
that restriction, and no REA decompilation is claimed. The session was closed.

This provides a better named reference for the next implementation stages.
It does not invalidate the reproduced OpenWrt ownership bug or establish
hardware QoS performance on V2802RH by itself.

Whole-release source/build/artifact inspection: [XZ005-G6 WIND3 GPL audit](XZ005-G6-WIND3-GPL-AUDIT.md).

Implementation follow-up: [queue ownership, rollback and diagnostics](HARDWARE-QOS-IMPLEMENTATION.md). The findings above describe the audited pre-fix source.
