# V2802RH hardware QoS audit, 2026-10-26

L2 hardware forwarding is working. OLT-driven hardware QoS is still absent.
This audit recovered additional queue ordering and rate-conversion behavior
from this unit's stock RTOS, and checked the shared queue bank against the
current backend's resource limits. No forwarding code or firmware changed.

## Evidence and boundaries

Source audited: `vsol-v2802rh`, through `409cb20d59`; the running firmware's
source is `c0bea668e5`.

| Artifact | SHA-256 |
|---|---|
| Exact-unit `v2802rh-full-backup.bin` | `4cc28e901cd311806a9103e61d8c7bd8fe32ac103ee5643db504b6a5b2d06086` |
| Decompressed stock RTOS | `8174f6378ae21b239b68b13e2eccf76d5212db8d386e823a865661ee7cbf74d2` |
| Same-family Realtek GPL `vmlinux` | `34a1ef23a866b74a18b7fed2a9005ac7995f9f3a49cd5c55caf79c614135e5cc` |

The carved LZMA input matches the backup at `0x61160`. Re-extraction with
7-Zip reproduces the saved RTOS hash and its declared 12,777,612-byte output.
This checks extraction provenance, not a vendor integrity signature.
Ghidra analysis uses the existing raw-image mapping at `0x80000000`, with
MIPS16e context for the stock functions below. The GPL ELF supplies named
SDK functions and register descriptors; it is not this board's RTOS.

Exports, assembly and audit helpers are kept locally under ignored
`tmp/v2802rh/l2-audit/`. No firmware was executed or hardware accessed during
this audit. Several decompiled functions have incomplete return/call
prototypes; conclusions below use identifiable field accesses and arithmetic,
with rate conversions corroborated by the instruction listings.

## 1. Queue allocation can fit without changing the tested T-CONT numbering

The backend has eight T-CONT slots and sixteen GEM slots
(`luna_xpon_hw.h`). Hardware T-CONTs 8–15 share physical QIDs 32–63.
OMCC retains SID 64, T-CONT 16 and QID 64. Current data QIDs 32–39 are
one fallback queue per T-CONT.

A first allocator can reserve those eight fallbacks and allocate at most
one additional queue per upstream GEM: **8 + 16 = 24 queues**, within the
32-entry bank. GEMs using the same logical priority queue can share an
allocation. Downstream-only GEMs need no upstream allocation. This bound
depends on preserving the current sixteen-GEM limit and one upstream queue
per GEM; split-queue SID aliases would need a different budget.

Therefore, the first implementation need not move T-CONTs into another bank
or preallocate all 64 advertised logical queues. Logical OMCI resources can
remain stable while only queues used by installed GEMs receive hardware
allocations. Unused logical queue settings still need to be retained.

SDK evidence: `_ponmac_schedulerQueue_get()` at `0x8013c8b8` finds the
owner by scanning T-CONT queue masks in the bank. Duplicate ownership is
ambiguous. `dal_rtl9602c_ponmac_flow2Queue_set()` at `0x8013c450` maps a
SID to the bank's physical QID. Queue release must respect every GEM and
fallback consumer; a bitmap alone is insufficient ownership state.

## 2. Logical priority must not be copied directly to a hardware QID

Stock's queue rebuild at `0x802a5e70` collects active queue records, sorts
them with comparator `0x802a2f38`, then assigns sequential physical queue
indices. The comparator places weighted queues before strict-priority
queues and orders distinct strict-priority values in descending numeric
order. Thus stock does not simply use the OMCI priority value as the QID.

G.988 defines related-port priority 0 as highest. The stock packing puts that
priority later in the physical sequence. An implementation must preserve
this ordering, then verify actual arbitration under contention; this static
trace is not a measurement of the hardware arbiter.

The stock update path at `0x802a6d20` obtains upstream weighted/strict policy
through helper `0x802a6c40`, which recognizes scheduler/T-CONT policy 2 as
weighted. Queue application at `0x802a57c0` sends a queue record containing
type, weight and rates to platform operation `0x17`.

## 3. Zero weights still require an explicit compatibility decision

The exact RTOS has different handling by direction. Its downstream branch
at `0x802a6d20` treats weights 0 and 1 as strict, and weights above 1 as
weighted. Its upstream branch copies the configured weight into queue
state; the examined handoff to operation `0x17` does not clamp it.

The same-family SDK's weighted-queue setter at `0x8013c5a8` rejects zero.
The remaining platform-dispatch handling of an upstream zero weight has
not been established. Do not infer that zero means either "weight one"
or "disable this active internet queue". The current Nokia provisioning
accepts zero-weight updates; changing that acceptance would be a regression.

G.988 specifies initial priority-queue weight 1. This is distinct from the
traffic scheduler's priority/weight attribute, whose initial value is 0.
The standard's priority, weight and rate definitions are in clauses
[9.2.10–9.2.12](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.988-202403-I!Amd1!PDF-E&lang=s&type=items).

## 4. Stock upstream and downstream rate paths use different conversions

The exact RTOS rate-update function at `0x802a9b58` performs:

| Path | Observed arithmetic | Destination |
|---|---|---|
| Upstream | `CIR >> 10`, `PIR >> 10` | Queue-state fields `+0x10/+0x14`, then queue application `0x802a57c0` |
| Downstream | `CIR / 125`, `PIR / 125` | Limiter wrapper `0x803f9598` |

Upstream instructions `0x802a9bd4/0x802a9bd8` and
`0x802a9be0/0x802a9be4` each implement the combined ten-bit shift.
Downstream instructions around `0x802a9c88–0x802a9c9e` load 125 and divide.
The downstream wrapper handles limiter deletion separately when its peak
rate argument is zero.

Stock's CLI table at `0x8061a734` and `0x8061a794` labels queue CIR/PIR
rates in 8-Kbps units. Handlers `0x801c6c60/0x801c6cbc` store the supplied
values directly in the staged queue configuration. The named SDK CIR/PIR
setters at `0x8013c674/0x8013c6d4` pass their arguments directly to 18-bit
register fields.

This establishes concrete stock conversions and excludes using one generic
conversion for every path. It does **not** yet establish calibrated physical
rate accuracy, a complete caller/parameter contract, or every zero/overflow
case. In particular, a positive upstream argument below 1024 becomes zero;
the correct factory/default behavior must be established before adopting
that rounding in a new backend. OMCI rate requests are in bytes/s and burst
requests in bytes; physical quantization must be reported separately.

## 5. Rate limits have different scopes; burst support is not established

Additional SDK controls supplement the earlier register map:

| Control | PON-IP offset | Scope |
|---|---|---|
| CIR / PIR | `0x2198 / 0x229c`, 4-byte stride | One physical queue, shared by its GEM consumers |
| Scheduler rate | `0x2584`, 4-byte stride, 17 entries | One hardware T-CONT/scheduler |
| PON egress rate | `0x25c8` | Shared PON egress |
| Scheduler/burst control | `0x2194` | Shared scheduler control, not independent per-GEM PBS |

The first three rate fields are 18 bits in the reference descriptors.
`dal_rtl9602c_ponmac_egrScheduleIdRate_set()` at `0x8013c308` confirms the
per-scheduler array; `dal_rtl9602c_ponmac_egrBandwidthCtrlRate_set()` at
`0x8013cd70` uses the global field. Their unit/range behavior must be checked
independently rather than borrowed from the queue setter.

A queue shaper cannot provide independent rate limits for two GEMs sharing
that queue. A T-CONT or port limiter is broader still. The inspected rate
path does not establish independent GEM burst control or complete downstream
colour policing. Global weighted mode at `0x2194` bit 19 must be managed as
a shared setting; its WRR/WFQ enum correspondence remains to be verified.

## 6. Backend state and update ordering need changes before hardware writes

- `luna_xpon_set_tcont_qos()` and `luna_xpon_set_gem_rate()` currently return
  success without retaining requests. Store requested state independently
  from installed hardware and expose requested/applied status.
- `luna_xpon_replace_service()` and `luna_xpon_service_same()` omit queue
  selection. Preserve it, compare it and resolve its mapping consistently
  for classifier traffic and CPU-originated traffic.
- `omci_agent_hw_update()` calls GEM rate callbacks **before** installing
  the GEM. A new implementation that requires an existing GEM slot would
  reject initial provisioning. Pending rate state must survive until the
  GEM/T-CONT is installed and be cleared on deletion/reset.
- `luna_xpon_tcont_bind()` can let a data binding share OMCC's QID 64 when
  its Alloc-ID matches the ONU ID. Data QoS must never rewrite OMCC's queue
  policy or rate through this special case.
- SDK queue deletion invalidates dependent SIDs before draining/removing
  the queue (`0x8013d6a8`). A live remap needs controlled isolation, bounded
  draining, readback and rollback. Preserve the fast OMCI reply path by
  deferring bulk reconciliation, as with the L2 worker.
- Current device operations do not describe downstream per-UNI queue
  scheduling or all queue buffer/back-pressure attributes. Implementing
  `set_tcont_qos()` alone would not provide full downstream hardware QoS.

## Implementation sequence and validation

1. Preserve pending queue/rate requests and GEM queue selection; expose
   requested/applied state. Keep identity, advertised resources and the
   working fallback forwarding behavior.
2. Add an ownership-aware allocator within QIDs 32–63, keeping 32–39 as
   fallbacks and QID 64 protected. Program only selected queues, with staged
   SID updates and cleanup. Check the sixteen-GEM bound and shared consumers.
3. Apply strict/weighted policy after resolving zero weights, hardware
   priority order and global WRR/WFQ mode. Do not enable guessed semantics.
4. Add rate controls after verifying conversion boundaries, zero/default
   behavior, measured rate quantum and limiter scope. Treat burst and
   downstream policing as separate supported capabilities.

Host checks should cover allocation exhaustion, shared queue release,
queue-only service changes, requests arriving before GEM creation, reset/
replay, OMCC protection and failure at each hardware update step. Board
checks need readback plus capped-flow, priority and weighted contention
tests while monitoring OMCI/O5 and CPU bypass.

Scheduling tests require distinct queues under the **same T-CONT**. The
current two services use different T-CONTs, so ordinary Internet speed
tests cannot establish their relative queue scheduler behavior. To distinguish
WRR from byte-weighted behavior, also use different frame sizes; equal-size
flows alone are insufficient. These measurements remain pending.

## Implementation, 2026-10-26

`luna_xpon.c` now applies upstream OLT QoS on the data T-CONT bank:

- T-CONT policy/weights and upstream GEM peak rates are retained per slot
  and GEM port ID, across OMCC reinstalls and in any arrival order; MIB
  reset clears them. The service queue (OMCI priority) is kept and compared.
- Per T-CONT, GEMs sharing a priority share a queue; a rated GEM gets its
  own. The least urgent queue is the slot's own QID 32–39, the others come
  from pool QIDs 40–63 in ascending order, so a strict T-CONT's most urgent
  queue has the highest QID (stock's packing). A T-CONT keeps the pool
  queues it holds. With at most 16 GEMs the 24-queue pool cannot run out.
- Strict policy → `WFQ_TYPE=0`; WRR → `WFQ_TYPE=1` with the OMCI weight,
  zero clamped to 1 (never starves a provisioned queue). A T-CONT without
  OLT QoS and more than one queue uses equal-weight WFQ. One queue keeps
  the install defaults (strict, weight 1).
- Upstream PIR is `bytes/s >> 10` (stock's conversion), minimum 1, maximum
  `0x3ffff` = unlimited; CIR stays 0. Burst and downstream rates are not
  applied and are logged once.
- New queues are drained before use; both old and new queues stay in the
  T-CONT's `SCH_QMAP` while SIDs move; released queues are drained and
  reset. Writes are read back. OMCC's T-CONT/QID 64 is never touched.
- Failures are logged (`T-CONT … QoS not applied`) and never returned to
  the OLT: data stays on its previous queue.

Host checks with simulated registers cover the current Nokia layout (no
register change), rates, strict ordering, WRR weights, sharing, release,
and that a SID never points at an unscheduled queue. Board contention
tests are still required; the current Nokia setup has one GEM per T-CONT,
so only a GEM peak rate, if the OLT sends one, changes behaviour today.
