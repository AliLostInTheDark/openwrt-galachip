# V2802RH L2 offload and hardware QoS plan

## Current status, 2026-10-26

L2 offload is now enabled by default and CPU bypass is demonstrated on both
LANs. The two-server capture confirms stable offload without new drop/error
counters. Hardware QoS remains unapplied; the next stage is queue allocation,
selection and scheduling, followed by verified rate conversion. The current
stock comparison and evidence gaps are in
[QOS-STOCK-AUDIT.md](QOS-STOCK-AUDIT.md). Recovery and disabled-path issues
remain separate open L2 checks. The baseline and staged plan below record
the original starting point.

## Baseline and scope

Start from `v2802rh-boot-36s`, commit `def58e73759a73d9a19a08f6c7e0208a42268da0`,
on `vsol-v2802rh`. The recorded, user-tested baseline passes traffic after
36 s, answers OMCI in about 1 ms median, and completed that boot without
OLT deactivations. L2 hardware offload remains disabled and untested.

Preserve the working identity/emulation, Nokia service seeding, advertised
queues/capabilities, GEM encryption, SID preservation, IVL learning and
deferred checkpoints. Earlier reductions of advertised resources and
refusals of QoS requests broke provisioning; do not repeat those changes.
Keep other boards working and keep unrelated boot, optics and UI work out
of these patches. No flashing or live configuration changes are part of
this planning step. Hardware validation will be RAM-booted and performed
with the user.

Relevant fixes: `57cc8a4d98` (Nokia provisioning), `2133cccb19` (AES),
`52ed7261be` (64 service slots), `c2f3b6a177` (service VLANs),
`63d351a61e` (SID preservation), `7b9623b182` and `f9029fa71f` (IVL),
`ceb993e3b9` (OMCI latency and boot).

## 1. Establish the resource map before implementation

Use the current code, exact V2802RH stock image/dumps and the matching
Realtek implementation to make an evidence table covering:

- OMCI T-CONT and queue entities -> Alloc-ID -> scheduler -> physical QID;
- GEM -> downstream CAM/AES entry -> upstream SID -> QID;
- UNI/p-bit/VLAN -> classifier rule/action -> GEM/SID;
- scheduler policy, priority order, weight encoding, CIR/PIR units,
  burst encoding, rounding, limits and disabled/unlimited values;
- downstream GEM policing and LAN egress scheduling, distinguishing them
  from a shared port-rate limiter.

Today the backend advertises eight queues per T-CONT but sends them to one
physical queue. `set_tcont_qos` and `set_gem_rate` accept requests without
applying them. Existing CIR/PIR and WFQ registers provide starting points,
not proof of a complete QoS implementation.

Resolve how different p-bits on one GEM select different physical queues.
One SID-to-QID mapping alone cannot establish eight independent queues.
Verify any priority-map or multiple-SID mechanism and its resource cost
before choosing the design. Reserve OMCC resources explicitly. Do not
assume that all advertised logical queues can be allocated simultaneously.

**Deliverable:** a source/address-backed resource and encoding table, with
unknowns marked and proposed allocation limits. No guessed register writes.

## 2. Audit and repair the existing L2 path while it stays disabled

Concrete pre-enable work:

- Bound planner output: `luna_xpon.c` accepts 64 services while the L2 output
  and priority arrays hold 16 groups. `services[n++]` currently has no
  capacity check. Test more than 16 distinct groups without modifying the
  working 64-service provisioning capacity.
- Validate the actual eight-p-bit-plus-default service set for both GEMs;
  preserve VLAN 100/660 separation and IVL on install and restore.
- Audit table ownership, WAN VLAN synchronization, failure rollback,
  generation changes and switchdev registration/removal together. Preserve
  the usable CPU path on a rejected mapping or failed enable.
- Check forwarding marks against actual RX descriptors, CPU copies and
  tag treatment. Matching a VLAN is not evidence that hardware forwarded
  that frame. Verify local management, CPU-originated traffic, broadcasts,
  MAC moves and bridge policy handling.
- Expose installed bindings, refusal reasons, readback and counter deltas
  without packet-by-packet logging or synchronous OMCI-path persistence.

Keep the current multicast-snooping restriction explicit. Do not silently
change bridge settings or claim MDB offload. VLAN translation, stacking,
multicast and split-GEM p-bit mappings remain explicit limitations until
implemented and tested individually.

**Deliverable:** one focused L2 repair/diagnostic patch, component compilation
and regression checks, plus exact enable/disable and capture commands.

## 3. Prove L2 forwarding on the board before adding QoS

Record an offload-off baseline, then compare offload-on and offload-off
again on the same image and traffic pattern. Test each LAN separately,
then simultaneous bidirectional traffic. Include ARP, IPv4, IPv6 and PPPoE
where available, untagged and tagged service traffic, MAC relearning and
VLAN isolation. Do not direct arbitrary test traffic into the ISP's
management VLAN; use a suitable peer/service for that test.

Evidence must include LAN/PON and GEM counters, CPU RX/TX deltas, CPU load,
throughput/loss and OMCI response timing. `active=1` and matching register
readback prove installation only. Use counter correlation to demonstrate
that data bypasses CPU forwarding without duplicate or missing packets.

Exercise disable/restore, service replacement/deletion, bridge membership
changes and re-ranging. Any failure first returns to the working CPU path.

**Gate:** stable traffic and management access, no unexpected OLT
deactivations, correct isolation, demonstrated CPU bypass in both directions,
and a successful return to the original forwarding behavior.

## 4. Implement hardware QoS in separate steps

After the resource map and L2 gate:

1. Allocate/map physical queues and select them from service priority,
   including the CPU fallback path. Keep logical OMCI entities stable and
   protect OMCC scheduling during allocation, replay and teardown.
2. Apply strict-priority and WRR scheduling using verified priority order,
   weights and zero-weight semantics. Preserve policy across GEM install,
   PON-MAC mode-set and re-ranging.
3. Apply supported upstream rate/burst controls with explicit unit
   conversion and readback. A shared T-CONT queue shaper must not be
   described as an independent per-GEM limiter.
4. Implement downstream per-GEM policing and LAN egress QoS only through
   verified hardware mechanisms. If a resource is shared or unsupported,
   report requested/applied state and the limitation separately while
   preserving the currently working Nokia provisioning behavior.

Validate rate conversion boundaries and resource cleanup locally. On the
board, test one capped flow, two competing classes, equal and unequal WRR
weights, strict priority under congestion, bursts and independent GEM/UNI
limits. Upstream results depend on OLT grants; local shaping cannot create
bandwidth the OLT has not granted. Repeat with offload disabled/enabled and
monitor control-plane latency under saturation.

**Gate:** measured caps within the verified hardware quantization, expected
scheduling under contention, correct limit scope, no cross-service impact,
and stable OMCI/O5 through replay and recovery.

## Delivery discipline

Keep L2 fixes, queue allocation/scheduling and rate controls in separate
reviewable patches. For each hardware step, provide the patch, checks run,
expected readback, user test commands, pass criteria and rollback. Update
results from actual captures. Compilation and host tests do not replace
hardware validation. Enable defaults only after the relevant hardware gate
passes. Implementation starts with phase 1 and the L2 bounds/ownership audit,
not another change to the working provisioning model.
