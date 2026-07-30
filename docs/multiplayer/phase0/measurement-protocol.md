# Phase 0 measurement protocol

## Clean timing gate

For every required device-count and OEM/API combination:

1. Keep the application host different from the logical DS host.
2. Warm the emulator and transport for 60 seconds; discard warm-up samples.
3. Run at least 10,000 completed synthetic command exchanges.
4. Preserve the core multiplayer receive timeout at 25 ms.
5. Record raw monotonic timestamps and immutable run metadata before deriving
   summary statistics.
6. Report p50, p95, p99, p99.9, maximum, misses, and sample count.

The candidate star passes a combination only when:

```text
at least 99.9% of SendCmd-to-RecvReplies exchanges finish before 25 ms
p99 is no greater than 20 ms
```

Every required clean combination must pass. Averages are diagnostic only and
cannot satisfy the gate.

## Timing points

Use one monotonic clock domain per device and correlate exchanged identifiers.
Record:

- logical DS host `SendCmd` entry and outbound queue residence;
- origin ENet service/egress delay;
- command leg from DS host to application host;
- application-host inbound queue and router residence;
- command leg from application host to each responder;
- responder inbound queue and emulator scheduling delay;
- responder `SendReply` entry and outbound queue residence;
- reply leg from responder to application host;
- application-host reply router residence;
- reply leg from application host to logical DS host;
- logical DS host inbound queue residence and `RecvReplies` return;
- deadline misses, drops, sequence rejection, and ENet barrier stalls.

Raw events use run ID, session ID, command sequence, logical member identity,
generation, stream ID, event kind, monotonic nanoseconds, queue depth, and
result. Device clock offsets are estimated at run start and end; no
cross-device duration is reported without offset/error bounds.

## Separate characterization

Contention and loss results never replace clean gate samples. Run and label:

- controlled same-LAN bulk traffic at low, medium, and saturation levels;
- deterministic application-layer loss/duplication/reordering where supported;
- natural Wi-Fi packet loss and retransmission counters;
- selected-network IPv4/link-property change during an active exchange;
- host loss and worker-owned teardown under saturated queues.

For API-29+ devices, run equivalent clean samples with
`WIFI_MODE_FULL_LOW_LATENCY` off and on. Record support/acquisition outcome,
tail-latency change, throughput impact, thermal/battery observations, and
whether production should retain the lock. An unsupported lock or acquisition
failure is a recorded condition, not a transport failure.

## Evidence and topology decision

Store raw traces outside Git when large and commit a manifest containing
checksums, collection commands, anonymized device IDs, and durable evidence
location. The Phase 0 report must record:

```text
selectedTopology
qualifiedMaxPlayers
testedDeviceMatrix
latencyGateResult
knownDeviceOrNetworkRestrictions
```

`qualifiedMaxPlayers` may not exceed the largest physically tested passing
device count and is initially capped at four. If the candidate star fails,
stop before post-gate work and choose one documented fallback: require
application host = DS host, direct DS-host data, hybrid/mesh data, or an
evidence-backed core-timeout investigation.
