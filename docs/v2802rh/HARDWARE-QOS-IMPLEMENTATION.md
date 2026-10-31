# V2802RH hardware QoS: ownership and rollback

Implemented 2026-10-30, following
[the QoS audit](HARDWARE-QOS-AUDIT-2026-10-30.md).

## Change

The queue planner reserves both old and proposed pool queues before MMIO.
A successful update verifies the scheduler mask and SID remaps before releasing
old queues. Failed updates restore every participating SID (including a write
that returned failure), the previous queue parameters and original mask.

If rollback cannot be verified, the T-CONT stops further QoS remaps and all
potentially referenced pool queues are quarantined. They are unavailable to
other T-CONTs until reboot. Cleanup failures also quarantine queues. Ordinary
channel reinstall detaches/drains/resets old healthy pool queues before clearing
installed caches, avoiding repeated-recovery resource leaks. Existing uncertain
queues remain quarantined across MIB reset/channel events.

Allocation exhaustion returns an application error instead of merging queues
and silently changing a requested rate limit. QoS callbacks retain their current
OLT compatibility behavior; internal errors are logged and exposed separately.
Successful source tests do not establish that an ISP OLT exercises this path.

GEMs of equal priority under a strict policy share by weight, because a rated
GEM's separate queue would otherwise outrank its equal.

Hardware helpers reject OMCC QID 64 and out-of-bank/invalid inputs before MMIO.
Scheduler masks now have checked readback; queue configuration additionally
checks CIR-zero readback. Existing drain/SID operations remain bounded. This
patch does not redesign the entire T-CONT/CAM teardown API or add deferred QoS
work, independent downstream policing, bursts, CIR or PCP-aware classification.

## Diagnostics

After booting the rebuilt firmware:

```sh
cat /sys/class/net/pon0/device/qos_status
```

The read-only file reports:

- Global quarantined queue bitmap; downstream GEM rates and bursts explicitly
  marked unsupported.
- T-CONT installed/OMCC flags, reserved pool, requested policy/enabled state,
  last QoS reconciliation error and rollback-fault flag.
- Upstream GEM SID, cached QID, requested bytes/s and cached PIR/weight/type.
- `unapplied_burst` and `unapplied_downstream_Bps` lines for a GEM whose OLT
  request has no hardware path; the OLT still sees success.
- `qos=unavailable` for a GEM on the T-CONT shared with the OMCC, which is not
  shaped.

`cached_*` is the last application cache, **not a fresh hardware-register dump**.
A failed rollback invalidates affected queue-cache entries. PIR is raw scheduler
encoding; no new rate-unit or line-throughput claim is introduced. A successful
rollback can leave old settings installed with a nonzero error for the newer
request. Read requested state, cache validity and errors together.

Healthy normal operation has `quarantined=0`, `error=0` and `faulted=0`.
A quarantine requires inspecting logs and rebooting before those resources can
be reused. Do not clear ownership manually while traffic may still reference it.

## Validation completed before full image packaging

- 156 checks using extracted production allocator/cleanup/reinstall functions
  with ASan/UBSan: transient failures at all eight hardware-operation positions,
  failed writes that mutate hardware, persistent rollback failure, cross-T-CONT
  ownership, strict/weighted/rate/sharing cases, exhaustion and cleanup.
- 22 checks using extracted hardware helpers: OMCC/bounds refusal before any
  MMIO, scheduler-mask readback errors and CIR readback errors.
- All 5,046 existing daemon tests passed with ASan/UBSan.
- `make -j32 target/linux/compile` passed; staged source matches edited source.
- Formatting/diff checks passed. Local harnesses/logs remain under
  `/tmp/v2802rh-qos-audit/` and are not firmware or publication files.

Full images are built as the delivery step. The running ONU has not been
modified or flashed. Hardware fault injection and multi-class scheduling
validation remain pending; do not describe these source tests as board tests.

## Hardware follow-up and rollback

After the user flashes, collect `qos_status`, `l2_offload`, O5/agent status and
kernel errors before changing QoS. Verify management and existing Internet
services first. Repeat ordinary reboot and physical-fiber recovery; confirm
no quarantine develops in normal operation. A controlled same-T-CONT/two-GEM
fixture is needed for actual scheduling/peak-rate contention tests.

Reverting the focused source commit restores the previous queue programmer;
rebuild full images before installing that rollback. Audit documents are in a
separate development commit and excluded from the source-only publication ref.
