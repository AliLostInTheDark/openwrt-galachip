# RTL9601D omcid provider model

2026-10-26. The shared engine handles OMCI messages, MIB operations and OLT
interoperability. The provider describes and programs its own resources.
The user's configured identity, four UNIs, versions, serial, OLT profile
and traffic-management option are preserved.

## Resource description

The typed topology response now supplies data GEM capacity, the ONU2-G
QoS flexibility bitmap, downstream queues per UNI, and permission to use
profile reference services, in addition to T-CONT/upstream queue resources.
The engine uses these values for its MIB instead of fixed GEM capacity
256 and fixed QoS capability bits.

The current RTL9601D backend describes the resources it implements:

| Resource | Implemented limit |
| --- | ---: |
| Data T-CONT bindings | 8, MEs 0x8000–0x8007 |
| Upstream queues per T-CONT | 1 |
| Data GEM bindings | 16 |
| Downstream queue MEs per UNI/VEIP | 1 |
| Advertised mutable QoS bitmap | 0 |
| Profile-created GEM/VLAN service graph | disabled |

These are backend limits, not claims about the chip's full capacity.
The MAC binding still uses hardware T-CONTs 8–15 and physical queues
32–39 on the RTL9602C register map used by RTL9601D. The OMCC remains
separate on T-CONT 16/queue 64. This mapping is the existing explicit
Luna mapping, not a translated packet-engine index from another SoC.

Selecting the Nokia profile still supplies its interoperability MEs and
quirks. On RTL9601D it no longer invents Alloc-IDs/GEMs 256/257, VLANs
100/660, mappers or bridge services. Those must come from OLT provisioning.
Other providers explicitly retain their prior profile-service behavior.
Both startup and runtime obtain topology through the same backend call;
a missing topology operation no longer substitutes a 32-T-CONT model.

## Binding lifecycle

T-CONT entity IDs map to stable backend slots. Out-of-range entities and
duplicate Alloc-IDs/GEM port IDs are rejected. Removing or changing a
T-CONT, or revoking its PLOAM allocation, withdraws dependent GEM hardware
and cached services. Regranting the allocation allows reconciliation to
reinstall them.

Services must reference an installed GEM and the matching granted
T-CONT/Alloc-ID. The backend checks direction and its single-queue limit.
PCP selects the service independently of its one hardware queue. Explicit
multi-queue scheduling, policing and multicast service installation remain
unsupported; they are not silently accepted.

## ABI and retained MIB

The netlink ABI is version **17**. Rebuild the kernel and all OMCI packages
together. Public headers and typed backend code are synchronized across
the three targets that share this interface. Package releases are bumped.

RAM checkpoints are version 4 and include the provider topology. Loading a
checkpoint from a different resource model is refused instead of replaying
its MIB into the current backend. The existing configuration is preserved.

## Verification

- Existing native omcid suite, including reference-profile behavior.
- Provider-specific tests: 16 GEM capacity, one queue/T-CONT, no invented
  Nokia services, and preservation of four emulated UNIs.
- Backend topology roundtrip and missing-topology refusal.
- Checkpoint refusal after provider-capacity change.
- Tests extracted from the actual Luna binding functions, with mocked MAC
  operations: grant revoke/regrant, allocation replacement, stable slots,
  service withdrawal, duplicate GEM rejection and unsupported services.
- MIPS compilation of Luna, typed backend and netlink core with the updated
  headers; complete native and MIPS daemon builds and synchronized ABI-copy
  checks.

The local extracted-function harness is under `tmp/v2802rh/omcid-audit/`
and is not part of the published source. No device access, firmware
installation or live forwarding test was performed. Full hardware QoS,
policing and LAN-to-PON offload remain separate implementation work.
