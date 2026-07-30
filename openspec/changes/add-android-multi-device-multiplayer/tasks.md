## 1. Fork and Phase 0 Setup

- [x] 1.1 Create the project-owned `melonDS-android` fork from `ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6`, add `rafaelvcaetano/melonDS-android` as `upstream`, and record both remotes.
- [x] 1.2 Create the project-owned `melonDS-android-lib` fork from `431ab4bd0003c4356e25fce89640ae1004579b9b`, add its source project as `upstream`, and record both remotes.
- [x] 1.3 Clone the Android parent fork into the implementation workspace, copy this OpenSpec change package into it, and make strict OpenSpec validation pass there before editing code.
- [x] 1.4 Update `.gitmodules` and the parent gitlink to use the project-owned native-core fork while retaining ENet `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`.
- [x] 1.5 Add a spike-only, disabled-by-default multiplayer build flag that cannot enable production lobby/UI work.
- [x] 1.6 Record clean baseline builds/tests for the parent debug variant and ARM64/x86_64 native targets.
- [x] 1.7 Name and document the content-free real-core multiplayer workflow used by Phase 0.
- [ ] 1.8 Select the two-, three-, and four-device OEM/API matrix, define how application host is kept different from DS host, and set initial `qualifiedMaxPlayers=4` while retaining wire-level `protocolMaximumPlayers=16`.
- [x] 1.9 Check in the measurement protocol: at least 10,000 clean exchanges per required combination, 99.9% before 25 ms, p99 at most 20 ms, plus separate contention/loss characterization.
- [x] 1.10 Before any low-latency Wi-Fi experiment, declare the normal `android.permission.WAKE_LOCK` permission and document that it is not a runtime prompt.

## 2. Phase 0 Selected-Network Transport and Teardown

- [x] 2.1 Implement only the framing-v1 envelope plus minimal hello, exact welcome with host identity, initial membership, rejection, DS frame, and barrier codecs required by the physical slice.
- [x] 2.2 Add a selected IPv4 Wi-Fi `Network` provider that does not require `NET_CAPABILITY_VALIDATED` and passes `Network.getNetworkHandle()` to native.
- [x] 2.3 Resolve spike hostnames through blocking `Network.getAllByName()` on an executor bounded to four executing/queued calls; implement logical generation cancellation, retain capacity until return, and pass only IPv4 bytes to native.
- [x] 2.4 Create ENet hosts with `enet_host_create(nullptr, ...)`, host peer count `qualifiedMaxPlayers - 1`, client peer count exactly 1, `maximumPacketSize=4096`, and `maximumWaitingData=65536`, then call `android_setsocknetwork()` before bind/connect.
- [x] 2.5 Manually bind application-host sockets to `ENET_HOST_ANY` plus the selected port, accept wildcard `enet_socket_get_address()` while verifying the port, display the selected `LinkProperties` IPv4 for direct join while retaining datagram-source discovery trust, and map bind errors to `NetworkBindFailed` or `PortUnavailable`.
- [x] 2.6 Add pinned ENet integration tests for selected-network inbound hosting, outbound joining, VPN-default routing, no-IPv4 DNS, port collision, and cleanup.
- [x] 2.7 Bound reliable outgoing retention at 65536 bytes per peer and 524288 bytes per host, retaining charges through ENet packet release.
- [x] 2.8 Limit connected-but-unwelcomed peers to `qualifiedMaxPlayers - 1` (initially 3, protocol ceiling 15) and disconnect a peer that does not send valid hello within 1500 ms.
- [x] 2.9 Implement a worker with nonblocking sockets, `enet_host_service()` waits no greater than 5 ms, atomic stop, and descriptor/condition wake.
- [x] 2.10 Keep DNS off the worker and treat cancellation as logical: ignore late resolver results whose session generation is stale without claiming the blocking call was interrupted.
- [x] 2.11 Make only the worker reset/destroy ENet and signal completion; join only after that signal and prohibit cross-thread force destruction.
- [x] 2.12 Prove under saturation that teardown completes within the 2000-ms SLO; if it breaches, remain `Stopping`, disable retries, keep facade/runtime gated and alive, report `ShutdownTimeout`, and enter `Failed` only after worker cleanup/join.

## 3. Phase 0 Core Adapter and Candidate-Star Relay

- [x] 3.1 Implement the stable `MPInterface` facade gate, in-flight guard, active-instance mask, receive-timeout retention, and active-instance replay needed by the slice.
- [x] 3.2 Implement a minimal copied-value LAN adapter enforcing 36..`0x948` regular/command/ack frames, 36..1024 payload replies, and only zero-length AID-0 default replies.
- [x] 3.3 Validate the internal frame-length field at bytes 10..11 and reject undersize, oversize, or inconsistent core frames before queueing.
- [x] 3.4 Implement one inbound command context per local instance and prevent delivery of a second command until reply consumption or expiry.
- [x] 3.5 Implement one outbound command context per local instance with deterministic supersession, exact reply selection, and ack correlation.
- [x] 3.6 Implement logical last-host context so `RecvHostPacket()` returns -1 on logical DS-host departure/inactivity even when application host remains.
- [x] 3.7 Zero all fifteen 1-KiB reply slots in `RecvReplies()` and wake every core wait with the specified values on stop/network failure.
- [x] 3.8 Implement independent control/regular/command/reply/ack/barrier sequence generators and preserve logical-origin class sequences across relay.
- [x] 3.9 At session creation assign host ID 0, generation 1, and a random nonzero local stream; include it in welcome and the initial membership, and keep clients in `Joining` until both identities/revision match.
- [x] 3.10 Implement host validation of client transport peer = logical origin, and client validation of transport peer = application host with preserved host-authorized logical origin.
- [x] 3.11 Implement the minimal A → host → B regular/command, B → host → A reply, and A → host → B ack paths without sender rewriting.
- [x] 3.12 Implement bounded command exchanges with endpoint-active expected responders, responder departure/inactivity removal, per-destination `RelayQueueDrop`, and asymmetric `RecvReplies()` completion: authoritative exhaustion only for application-host-local DS commands.
- [x] 3.13 Add native tests for welcome/snapshot identity disagreement, relayed origin validation, frame-before-membership ordering, stale client membership during reply wait, application host ≠ DS host, overlapping commands, stale contexts, and wait wake-up.

## 4. Phase 0 Physical Feasibility Gate

- [x] 4.1 Instrument monotonic `SendCmd` entry through `RecvReplies` return without changing the 25-ms core timeout.
- [x] 4.2 Instrument every relay leg, ENet service delay, both queue residences, responder emulator scheduling delay, and completion/deadline misses.
- [x] 4.3 On API 29+, add controller/lifecycle-owned spike-only A/B acquisition of `WIFI_MODE_FULL_LOW_LATENCY`, scoped to foreground/screen-on measurement and released after every run/failure/teardown; record unsupported/acquisition failure without failing transport.
- [ ] 4.4 Run the two-device physical matrix with application host ≠ DS host and record raw traces plus p50/p95/p99.
- [ ] 4.5 Run the three-device physical matrix with application host ≠ DS host and record raw traces plus p50/p95/p99.
- [ ] 4.6 Run the four-device physical matrix with application host ≠ DS host and record raw traces plus p50/p95/p99.
- [ ] 4.7 Repeat required clean runs across the selected OEM/API combinations and record whether 99.9% finish before 25 ms and p99 is at most 20 ms.
- [ ] 4.8 Run controlled Wi-Fi contention and packet-loss characterization separately from the clean pass/fail percentiles.
- [ ] 4.9 Compare low-latency Wi-Fi mode on/off tail latency, battery/throughput effects, and device support.
- [ ] 4.10 Demonstrate selected-network IPv4/link-property change failure and worker-owned teardown on physical devices.
- [ ] 4.11 Publish a Phase 0 report with raw-data location, methodology, tested device matrix, distributions, misses, queue/leg breakdown, known device/network restrictions, low-latency lock decision, and limitations.
- [ ] 4.12 Record `selectedTopology`, `qualifiedMaxPlayers` (no more than 4 from this matrix), `testedDeviceMatrix`, `latencyGateResult`, and `knownDeviceOrNetworkRestrictions`; choose star, application host = DS host, direct DS-host data, hybrid/mesh data, or an evidence-backed timeout investigation. If star does not pass, stop and update proposal/design/specs/tasks before section 5.

## 5. Post-Gate Complete Protocol Codec

- [ ] 5.1 Confirm task 4.12 approved the topology represented by the current artifacts; do not continue if the package requires amendment.
- [ ] 5.2 Add complete fixed-width constants for protocol 2.0, core epoch, features, messages, error/reason codes, ports, limits, and six sequence classes.
- [ ] 5.3 Implement checked little-endian primitive readers/writers that never serialize native structures.
- [ ] 5.4 Implement exact `ClientHello`, `ServerWelcome` with assigned-client plus ID-0 host identity, shared-framing `JoinRejected`, and `LegacyOrUnknownProtocol` handling.
- [ ] 5.5 Implement checked discovery, membership, endpoint, leave, exact four-byte uint16-reason/uint16-zero `HostShutdown`, DS frame, and channel-barrier codecs.
- [ ] 5.6 Implement canonical string validation and all reserved/enum/total-length checks.
- [ ] 5.7 Check in hand-reviewed golden vectors for every message, including `HostShutdown` reserved-byte rejection and welcome host identity, every sequence class, boundary string, frame boundary, and shared-framing rejection.
- [ ] 5.8 Add round-trip, truncation-at-every-offset, trailing-byte, invalid-enum, invalid-UTF-8, and declared-length tests.
- [ ] 5.9 Add corpus fuzz targets and pass ASan, UBSan, and leak detection without unbounded allocation.
- [ ] 5.10 Verify identical golden vectors in host x86_64 and Android ARM64/x86_64 tests.

## 6. Post-Gate Bounded Runtime, Router, and Worker

- [ ] 6.1 Implement production fake/real monotonic clock, datagram, resolver-result, secure-random, and network-binding seams.
- [ ] 6.2 Implement the 64-entry/64-KiB local command queue with named `Leave` and `Cancel` reservations plus out-of-band stop.
- [ ] 6.3 Implement per-peer 64-entry/64-KiB control budgets and retain reliable charges until ENet release.
- [ ] 6.4 Implement the one-slot newer-revision membership coalescer and both 256-entry/512-KiB age-bounded DS queues.
- [ ] 6.5 Implement the 128-entry/32-KiB diagnostic queue and per-destination relay counters.
- [ ] 6.6 Enforce bounds of 16 exchanges, 64 discovery entries, `qualifiedMaxPlayers - 1` pending peers, 64 source records, 4 blocking resolver calls, 96 sequence windows, and `qualifiedMaxPlayers` peer diagnostics.
- [ ] 6.7 Implement secure session IDs, fixed host ID 0/generation 1/random local stream, client ID/generation assignment, and stream IDs unique among live identities.
- [ ] 6.8 Implement authoritative membership snapshots, welcome/first-snapshot host/client identity agreement, endpoint activity, lock state, fresh joins, and revision filtering.
- [ ] 6.9 Complete topology-approved routing, reply/AID collision rules, expected-responder updates, and exchange cleanup.
- [ ] 6.10 Implement channel-0 reliable control and channel-1 `UNRELIABLE_FRAGMENT` DS payloads.
- [ ] 6.11 Implement the pre-60,000 reliable barrier and test the rare bounded synchronization stall without DS payload retransmission/promotion.
- [ ] 6.12 Complete discovery broadcast, source-address trust, five-second expiry, direct fallback, and bounded legacy-advertisement labeling.
- [ ] 6.13 Complete admission/rate limits, malformed thresholds, hello timeout, peer timeout, and bounded worker failure paths.
- [ ] 6.14 Add deterministic loss/delay/jitter/duplicate/reorder, queue saturation, sequence wrap, exchange expiry, and teardown tests.
- [ ] 6.15 Add real ENet loopback tests for two, three, four, and sixteen logical members while proving production admission/advertisements remain capped at `qualifiedMaxPlayers`.

## 7. Post-Gate Session Manager and Facade Hardening

- [ ] 7.1 Complete backend quiescence: gate, drain, end active instances, worker cleanup signal/join, install, timeout copy, begin replay, reopen.
- [ ] 7.2 Stress `LocalMP → LAN → LocalMP` with active endpoint zero and concurrent core calls.
- [ ] 7.3 Implement the exact seven-state session transition table with `Stopping(terminalTarget)`, no retryable `Failed` before cleanup/join/facade restoration, and independent endpoint, lock, revision, and error fields.
- [ ] 7.4 Restore `LocalMP` on failure only after worker completion and pause emulation for fatal live-session failures.
- [ ] 7.5 Require modal acknowledgement before isolated gameplay resumes after host/network/protocol/worker failure.
- [ ] 7.6 Complete immutable native snapshots for state, identities, members, discovery, endpoint, diagnostics, and typed errors.
- [ ] 7.7 Add exhaustive transition, cancellation, retry, invalid-command, shutdown-SLO, live-old-worker exclusion, and model-based state-machine tests.
- [ ] 7.8 Add facade/context tests for duplicate Begin/End, timeout replay, logical host loss, overlapping commands, and shutdown wake-up.

## 8. Post-Gate JNI and Android Network Policy

- [ ] 8.1 Add Kotlin/JNI command, admission, error, discovery, member, diagnostic, and immutable snapshot types.
- [ ] 8.2 Add nonblocking JNI entry points and a one-dirty-bit/one-outstanding-event snapshot invalidation handshake.
- [ ] 8.3 Expose snapshots through a conflated `StateFlow` without adding multiplayer events to an unbounded `SharedFlow`.
- [ ] 8.4 Prove native retains no Activity, ViewModel, UI object, `JNIEnv*`, or Java callback.
- [ ] 8.5 Add `ACCESS_NETWORK_STATE` and retain the target-SDK-36-or-lower policy without `ACCESS_LOCAL_NETWORK`.
- [ ] 8.6 Test Android 16 `RESTRICT_LOCAL_NETWORK` disabled/enabled, temporary `NEARBY_WIFI_DEVICES` behavior, UDP `EPERM`, and `LocalNetworkPermissionDenied`.
- [ ] 8.7 Add the target-SDK-37-or-higher manifest/runtime `ACCESS_LOCAL_NETWORK` path, rationale, denial, and revocation behavior behind SDK guards.
- [ ] 8.8 Disable discovery, host, and direct join whenever required local-network permission is denied.
- [ ] 8.9 Observe ordered capabilities/link properties and API-29+-guarded blocked status; on API 24–28 rely on permission checks, `onLost`, link/capability changes, and socket errors.
- [ ] 8.10 Add JVM/instrumentation tests for unvalidated Wi-Fi, VPN default, selected-network blocking DNS/logical cancellation, wildcard host bind/direct address, port collision, permission versions, callback API guards, and same-handle DHCP change.

## 9. Post-Gate Retained Repository and UI

- [ ] 9.1 Add an activity-retained multiplayer repository beside `EmulatorSession` and scope cleanup to emulator-runtime ownership.
- [ ] 9.2 Integrate conflated invalidation with `AndroidEmulatorManager` and prove activity recreation creates no duplicate operation/worker.
- [ ] 9.3 Add Multiplayer to ROM and firmware pause menus and route it through `EmulatorViewModel`.
- [ ] 9.4 Implement the Compose host/join setup with persisted player/session/max/port/discovery/diagnostic preferences while capping maximum-player choices at `qualifiedMaxPlayers`.
- [ ] 9.5 Implement discovered and direct join input, selected-network permission rationale, and typed validation.
- [ ] 9.6 Implement lobby members, endpoint activity, ping, application-host marker, lock/unlock, direct endpoint, and diagnostics.
- [ ] 9.7 Implement cancel/leave/retry/stopping and fatal modal acknowledgement flows.
- [ ] 9.8 Keep emulation paused during setup and fatal failure; resume only on explicit user action.
- [ ] 9.9 Rehydrate every overlay state after activity recreation without replaying commands.
- [ ] 9.10 Add localization, accessibility semantics, controller/keyboard navigation, focus order, and large-font behavior.
- [ ] 9.11 Add ViewModel/reducer and Compose UI tests for every state, permission version, error, and lifecycle path.

## 10. Post-Gate Hardening and Release Evidence

- [ ] 10.1 Run all parent unit tests, native tests, lint, debug/nightly builds, and supported-ABI packaging.
- [ ] 10.2 Run codec/router/queues/facade/worker under ASan/UBSan/LSan and TSan where supported.
- [ ] 10.3 Run deterministic fault suites with fixed seeds and no wall-clock sleeps.
- [ ] 10.4 Soak repeated setup/cancel/leave/backend switches while checking threads, sockets, packets, descriptors, resolver jobs, and retained objects.
- [ ] 10.5 Verify x86_64 Android emulator to ARM64 physical protocol interoperability where networking permits.
- [ ] 10.6 Qualify the release on simultaneous physical devices equal to `qualifiedMaxPlayers` (initially four) across multiple APIs/OEM Wi-Fi stacks under the approved topology; require an updated same-size capacity run before raising the value to eight or sixteen.
- [ ] 10.7 Qualify the named content-free workflow and separately obtained retail-title cases outside automated fixtures.
- [ ] 10.8 Record active-traffic soak, application-host loss, logical DS-host departure, and repeated fresh joins.
- [ ] 10.9 Record above-MTU loss and barrier rollover: DS payloads are not retransmitted/promoted, while rare bounded barrier synchronization stalls are measured.
- [ ] 10.10 Document AP isolation, discovery/direct fallback, qualified versus protocol capacity, permission-version behavior, legacy incompatibility, low-latency lock/`WAKE_LOCK` retention decision, and troubleshooting.
- [ ] 10.11 Enable release only after every mandatory automated/physical item has dated evidence and no correctness blocker.
- [ ] 10.12 Pin the reviewed native-core commit in the parent release branch and document feature-flag/gitlink rollback.
