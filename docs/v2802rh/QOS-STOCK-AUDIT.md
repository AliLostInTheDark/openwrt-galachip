# V2802RH stock comparison and hardware QoS status, 2026-10-26

The subsequent exact-RTOS queue/rate trace is recorded in
[HARDWARE-QOS-AUDIT.md](HARDWARE-QOS-AUDIT.md), including newly recovered
rate conversions, priority packing and the bounded allocator design.

## Two-server result

The replacement `/home/ali/Desktop/l2-speed.log` has SHA-256
`594d56b0b36492ac0d873348bdd79f1845a4033e059355b111c5c2ec3a4f2ec9`.
It contains two complete 60-sample captures. Both are LAN2 tests; LAN1
byte counters do not change. The user identifies the first server as having
the larger speed swings, and the second as having steady download with
smaller upload swings near 300 Mbps.

| Measured delta | First server | Second server |
|---|---:|---:|
| Captured duration | 65.34 s | 65.59 s |
| LAN2 RX bytes | 614,024,123 | 606,109,675 |
| LAN2 TX bytes | 608,113,002 | 619,704,874 |
| Combined CPU-port RX/TX bytes | 2,538 | 25,119 |
| New CRC/symbol errors, RX drops or TX discards, all ports | 0 | 0 |
| New bridge-discard counts, all ports | 0 | 0 |
| LAN2 TX pause frames | 142 | 286 |
| Average CPU utilization, including capture | 10.9% | 17.2% |

All 120 samples retain `active=1 services=2 error=0 gated=0 generation=366
retries=0`; no `cpu_only` counter increases. CPU traffic is negligible
compared with 2.45 GB of LAN2 data. The speed variation is not caused by
bulk CPU forwarding or offload cycling in these runs.

Pause frames occur primarily during upload. The second server produces
more pause frames despite its smoother reported result, including 156
at upload onset. Counts alone cannot establish pause duration or its
throughput impact. Server/path/client pacing and OLT grant timing remain
plausible; neither server overhead nor a specific local queue defect is
proved. These are MAC-byte samples, not speed-test payload measurements.

## Comparison against this unit's stock capture

Exact-board evidence: `/home/ali/Desktop/register-stock-online.log`, SHA-256
`91f6cc7ab6b3b814a643d84410c397c343e38c85b804e04b454ae5b9f2bc365c`.
The table compares that dump with source at `c0bea668e5`; the new speed
captures contain counters, not fresh configuration-register readbacks.

| Control | Stock | Current driver program |
|---|---|---|
| LAN2 forced MAC ability | `0x76` | `0x76`, including TX/RX pause |
| Auxiliary FC thresholds at `0x230f0/f4/f8` | `0x00400034 / 0x00f000ea / 0x00400034` | Same three words in `rtl9602c_cpu_tag_arm()` |
| Switch meter timing `0x25000` | `0x12bbd` | Same word/fields |
| UM/UC/BC/DHCP storm enables | `0x7` each | Same enables and meter-index words |
| LAN/CPU and PON frame limits | 1600 / 2031 bytes | Same limits |
| PON token timing `0xf02190` | `0x956e` | Same word/fields |
| PON scheduler control `0xf02194` | `0x66000` | Same word under default `sch_ctrl_stock` |
| Data T-CONT queue masks | T0: `0x3c`; T1: `0x3c0` | T8: `0x1`; T9: `0x2` for the tested two bindings |

Stock therefore enables queue bits 2–5 and 6–9 in its two data T-CONTs.
Our backend uses physical QIDs 32 and 33, one each in the shared bank for
T-CONTs 8–15. Different hardware IDs are not themselves an error, but queue
allocation and scheduling are not equivalent to stock.

The stock dump prints `Unexpected Case` for SID-to-QID, queue CIR/PIR,
queue type and weight arrays (`0xf020f8`, `0xf02198`, `0xf0229c`,
`0xf023e8`, `0xf023f8`). It does not establish which of the four stock
queues carries each GEM, or its actual rate/weight/type. The earlier
OpenWrt register dump also predates the current PON/offload setup; its
differences are not evidence of this running image's configuration.

## Hardware QoS is not implemented yet

`luna_xpon_set_tcont_qos()` and `luna_xpon_set_gem_rate()` still accept
requests without applying or retaining them. `luna_xpon_replace_service()`
discards the normalized service queue; GEM install maps its SID to the
T-CONT's fixed QID. Current queues have maximum raw PIR, CIR 0 and fixed
type/weight. Hardware forwarding is working, but it is not OLT-driven QoS.

The next implementation stage is queue selection and scheduling:

1. Retain requested T-CONT/PQ/GEM state and resolve the GEM's configured
   queue. Allocate physical queues from the shared banks, reserving OMCC
   SID/T-CONT/QID 64/16/64 and preserving advertised logical resources.
2. Apply the verified scheduling policy and weights with readback and
   replay/cleanup. Establish strict-priority ordering and the accepted OMCI
   zero-weight behavior before programming them. Compare requested/applied
   state explicitly; do not silently label accepted requests as applied.
3. Add rate/burst handling separately after verifying units, quantization
   and scope. The backend API uses bytes/s and bytes; the retained Realtek
   SDK decompilation passes raw CIR/PIR values directly to 18-bit registers
   and does not establish that conversion. A shared queue shaper is not an
   independent GEM limiter, nor does it supply downstream policing.

The retained SDK exports and register descriptors are same-family Realtek
evidence, not this board's RTOS implementation. No guessed rate conversion,
pause-disable change or firmware rebuild follows from this audit. Details
of bank capacity and unresolved encodings remain in [L2-QOS-AUDIT.md](L2-QOS-AUDIT.md).
