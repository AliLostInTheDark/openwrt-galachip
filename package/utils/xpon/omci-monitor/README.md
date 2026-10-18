# OMCI userspace migration: wire core and diagnostics

This is the first migration stage, not a second OMCI responder. The kernel
still owns the live MIB, transactions, service resolution, alarms and profiles.
`omci-monitor` binds a read-only observer and cannot use OMCI TX. It neither
disables the working agent nor changes registration. It is optional, has no
init service and is not added to the default 4 MiB image.

The optional sibling `libomci-protocol` now also builds the ME catalogue,
attribute codec and OLT profiles natively. Both kernel and library use the
same sources. This is reusable protocol code, not live MIB ownership or an
exclusive userspace responder.

The original GPL-2.0-only wire codec is shared byte-for-byte with
`net/xpon/omci/wire.{c,h}`. Keep those copies synchronized. Existing
`omci-agent`/LuCI ancestry and Matheus Sampaio Queiroga (@Sirherobrine23)
credits are unchanged. This package adds diagnostics, not a new claim of
authorship over that work.

## Usage

Requires matching OMCI UAPI v15 and root for the observer bind:

```sh
omci-monitor -d 0
omci-monitor -d 0 -c /tmp/private-omci.capture
omci-monitor -r /tmp/private-omci.capture
```

Default JSON output contains headers, generation, RX sequence, flags, and
Create/Delete/Set/Get response codes, not attribute values. Live device
identity can still be inferred from entity IDs and timing. Raw capture is
explicit: the destination must not exist and is created mode 0600. It may
contain passwords and identity. Never post an unsanitized capture or add it
to Git. Ctrl-C closes the socket and releases the observer. A busy observer
slot returns an error; this tool never evicts another owner.
Raw capture stops around 1 MiB (at the end of the current record) to avoid
exhausting the small ONU's RAM. Normal stdout redirection is not size-limited.

Capture format is one PDU per line:

```text
rx GENERATION RX_SEQUENCE FLAGS HEX_PDU
tx GENERATION 0 0 HEX_PDU
```

`#` metadata lines retain local monotonic timestamps, status and transport-loss
indications. Framing-only replay skips metadata; inspect these lines separately
and preserve them when creating a sanitized fixture. JSON `ms` on replay is
the replay process's clock, not the original capture timestamp.

TX means the driver accepted the response, not that the OLT received it.
Trailer generation/checking stays in the driver (software CRC in the GC1601
backend, despite the historical `HW_MIC` capability name). The codec checks
framing, not every G.988 semantic or MIC. TX trace is emitted before the
corresponding RX observation because the current kernel responds first;
do not use record arrival order as wire order or calculate RTT from it.
`trace_dropped`, RX sequence gaps, and `transport_loss` expose incomplete
captures. No timeout or interoperability conclusion is valid across a loss
or session-generation change. Offline replay validates framing only; it does
not emulate provisioning or prove OLT compatibility.

Host tests (no full image build):

```sh
make -C tests test
```

## Next migration stages

1. Introduce owner-qualified, generation-checked hardware object operations:
   topology/capabilities, T-CONT, GEM, QoS, UNI, service replace/delete and
   telemetry. Validate and report each hardware error. Do not expose raw MMIO.
2. The ME catalogue and profile policy now have a shared native build. Move
   the remaining MIB, transaction duplicate cache, table
   snapshots and service resolver into a native daemon. Preserve the public
   UCI/ubus/LuCI interface via a daemon control socket. Only one component may
   own the authoritative MIB and respond on an OMCC at a time.
3. Move profiles/quirks, alarms and PM using sanitized request/response replay
   fixtures and backend-mocked tests. Profiles must advertise implemented
   behavior; vendor names alone do not certify support. Complex changes still
   need code, not an arbitrary JSON success rule.
4. Add an explicit exclusive userspace-owner mode only once those backend
   operations and recovery/replay exist. Socket loss must disable admission of
   new provisioning, preserve laser safety and avoid silently starting a
   second responder with a divergent MIB. Flip the default after hardware
   GPON regression tests, not as part of the diagnostic rollout.

EPON already uses `epon-oamd` for IEEE/CTC OAM. Keep MPCP, registration,
generation, timed TX holds and optical safety in the kernel. Do not route
EPON OAM through this GPON codec. Test each daemon with its actual OLT frames.
