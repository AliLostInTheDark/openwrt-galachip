# Application-transfer diagnostics

Optional package, not a default image dependency. It reads an allowlist of
driver snapshots and `/proc` counters for up to 60 samples. It does not toggle
offload, rerange, reset counters, read credentials or run a speed test. Some
hardware snapshot reads still use the driver's diagnostic register interface;
collection has overhead. Compare tests with identical sampling, and check once
without sampling if the throughput changes noticeably.

```sh
umask 077
xpon-diagnostics 30 > /tmp/g7-application.txt
# Repeat while Speedtest runs, same PC, uplink, IP family and topology:
xpon-diagnostics 30 > /tmp/g7-speedtest.txt
```

Record the client-reported unit (MB/s versus Mbit/s), server, IPv4/IPv6, single
versus parallel connections and upload/download. Capture the same application
on the stock ONU under the same conditions. On the client, take TCP statistics
and a capture or `ss -ti` during the transfer to check retransmission, RTT,
congestion window and path MTU. The ONU's TCP counters describe connections
terminating on the ONU, not bridged client flows; they cannot establish client
TCP loss. A software capture on `pon0` misses hardware-forwarded SFU traffic.

Interpretation:

- Rising CPU-path bytes plus CPU load suggests offload misses or unsupported
  traffic; inspect the learned/programmed VLAN/PCP keys and frame format.
- Rising descriptor, buffer, CRC or invalid-length counters supports a local
  datapath problem. Compare deltas, not lifetime totals.
- Programmed flow entries are not proof of hardware hits. Existing hardware
  hit counters are unavailable. Zero software drops do not prove no ASIC drop.
- Full parallel Speedtest with slow single-connection TCP can reflect loss,
  latency, remote limits or different IPv4/IPv6 paths. It does not by itself
  prove either Google throttling or a G7 defect.

Keep captures private. The allowlist avoids credentials, but interface names,
MAC addresses, VLANs and physical-link details remain in driver snapshots.
