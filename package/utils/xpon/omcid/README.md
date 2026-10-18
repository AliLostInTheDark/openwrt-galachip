# Native GPON OMCI responder

`omcid` runs the existing OMCI MIB, transaction, service-resolution, alarm
and PM engine in one userspace event loop. The kernel retains PLOAM,
activation, encryption, optical safety and hardware programming. EPON
continues to use `epon-oamd`; it is not an OMCI protocol mode.

It is the only GPON OMCI responder; the kernel has no OMCI engine of its
own. `/etc/init.d/omcid` (START=11) binds before the ONU ranges; it is
skipped when `xpon.main.mode` is `epon`.
UAPI v16 kernel and userspace packages must be installed together.

`omcictl`, ubus and LuCI administration use the root-only
`/var/run/omcid.sock`. `omcictl status` identifies the responder as
`userspace` or `orphaned` (`kernel` only before omcid first binds).

## Tracing

The kernel accepts one owner, so `omci-monitor` cannot attach while omcid
runs. `kill -USR1 $(pidof omcid)` starts a trace in `/tmp/omcid-trace.txt`
and a second signal stops it; it also stops at 1 MiB. Read it with
`omci-monitor -r /tmp/omcid-trace.txt`. It holds raw OMCI: keep it private.
`OMCID_DEBUG=1` additionally logs every hardware write.

Without tracing, each OMCC activation leaves one info line in `logread`
once the OLT has been quiet for 10 s (at most 120 s), for example
`omcid: OLT after 116 s dark: MIB reset and full reprovision; 40 messages ...`
or `... after 11 s dark: audit only ...`. It counts the OLT's MIB Reset,
upload, create, set, delete and get requests and ends with the MIB sync.

## Failure and restart

Every acknowledged transaction and local mutation is checkpointed in
`/tmp/omcid-0.state` with private permissions and atomic replacement. The
checkpoint includes duplicate-response state, table snapshots, profiles,
the MIB and protocol timers. It contains credentials: never publish it.
It is build/architecture-local and deliberately not stored on flash.

Restart the same command on the same boot to load the validated checkpoint.
The kernel's userspace latch survives socket closure: it does not restart
its old responder after a daemon crash. A restart withdraws OMCI datapath
resources and replays the checkpoint, preserving PLOAM grants, keys and
the OMCC. Missing, invalid or mismatched state fails closed; reboot and
reprovision instead of deleting a live checkpoint to force a new MIB.

Generation changes invalidate transaction/table caches while retaining the
MIB for service reconciliation. Resource/grant changes request daemon
replay. Lost RX records and ambiguous transport failures stop the daemon
without acknowledging possibly uncommitted state. The OLT may retransmit
an unacknowledged request after restart.

The event loop only runs OMCI traffic, requested administration and protocol
alarm/PM/reboot timers. There is no sample collector or mean/median history.
Measured on the XZ000-G7 with an Airtel (Nokia) provisioned MIB: about
1.1 MiB RSS (1.4 MiB peak) and a 270 KiB checkpoint in RAM.

## Development

Run `make -C tests test`. The engine started as the in-kernel agent and
now lives only here. The definitions both sides still use (`internal.h`,
`backend.c`, `device.h`, the backend UAPI) stay byte-identical to the target
overlay; `tests/check-sync.sh` checks that. Extend the shared engine/profile sources, not an independent
vendor fork. Hardware limits and unsupported shaping remain unchanged.
The native daemon does not ask the kernel to approve OLT profiles/quirks;
those are userspace policy. A genuinely new hardware operation still needs
a backend extension, not a userspace register-write escape hatch.

The original omci-agent and LuCI package work by Matheus Sampaio Queiroga
(@Sirherobrine23) is retained and credited in the existing package notices.
Engine sources retain their GPL-2.0-only notices. `int-log-native.c` retains
Christoph Pfister's LGPL-2.1-or-later implementation and copyright; the
Linux jhash header retains its original notice.
