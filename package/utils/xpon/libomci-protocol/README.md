# Shared OMCI protocol library

`libomci-protocol` builds the existing kernel agent's managed-entity catalogue,
attribute codec, wire framing, OLT profiles and parsed OLT-G/VLAN handlers as
native userspace code. It is optional, GC1601-only, has no init service and is
not selected by default.

The kernel remains the sole OMCI responder. This library does not yet own the
live MIB, duplicate transactions, table snapshots, service resolution, alarms
or PM. It does not add a raw-register API or change optical recovery. Profile
names describe existing policy; they are not a claim of universal OLT support
or complete G.988 coverage.

Sources and descriptors are shared byte-for-byte with the kernel overlay.
`me-kernel.c` adapts the shared functions to the existing kernel state without
additional MIB copies. Valid-profile attribute numbering and packing are
preserved; invalid profile values are rejected before a bitmap shift.

`attributes.h` exposes OLT-G updates, class 84 VLAN filters, class 171 scalar
attributes/DSCP maps and keyed table insertion, replacement and deletion.
Its Create helpers consume the decoded object's fixed-offset data, not the
packed Create payload; run the ME Create decoder first. Set helpers consume
the packed selected attributes. Unknown masks are checked by the ME codec
before these class-specific helpers, just as in the kernel agent.

Decode into a staged object and commit only after successful validation.
Create/Set decoding can modify earlier attributes before rejecting a later
one, matching the kernel's existing behavior. The library allocates no MIB
and starts no threads or timers. Callers own serialization and object lifetime.

Development headers install under `/usr/include/omci/`; consumers add
`-I/usr/include/omci` and link `-lomci-protocol`. These C structures are an
internal library interface, not a packed netlink ABI. OMCI transport stays at
UAPI v15.

Host checks, including source-copy consistency:

```sh
make -C package/utils/xpon/libomci-protocol/tests test
```

The original GPL-2.0-only code and existing authorship are retained. The
omci-agent and luci-app-omci packages originate from the Airoha GPON OMCI work
of Matheus Sampaio Queiroga (@Sirherobrine23); this extraction does not replace
that attribution. It adds a shared native build, not a new implementation of
the original protocol fundamentals.
