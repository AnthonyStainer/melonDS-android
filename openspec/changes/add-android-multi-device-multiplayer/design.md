## Context

The corrected target is `rafaelvcaetano/melonDS-android`, not desktop melonDS. The parent repository is pinned at `ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6`; its `melonDS-android-lib` native-core submodule is pinned at `431ab4bd0003c4356e25fce89640ae1004579b9b`; and its ENet submodule is ENet 1.3.18 at `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`.

The Android app currently lists local multiplayer as missing. It already:

- builds and links ENet;
- calls `MPInterface::Begin`, `End`, send, and receive methods from the Android platform layer;
- calls `MPInterface::Process` from the native emulator loop;
- has an activity-retained emulator manager/session and a native-to-Kotlin message pipe;
- uses an emulator pause menu and supports Compose overlays;
- declares `INTERNET`, Wi-Fi, network-change, and nearby-Wi-Fi permissions; and
- contains preliminary multiplayer strings but no complete host/join feature.

The pinned native LAN implementation is not suitable to expose directly. It transmits native C++ structures, blocks for up to five seconds during join, forms a client mesh, retains mutable `ENetPacket*` values in an unbounded receive queue, and sends all DS frames unsequenced. `MPInterface::Set` also destroys a global `unique_ptr` without quiescing emulator-thread callers or replaying active instances.

Android adds further constraints: activity recreation must not destroy a live native session, sockets must use the selected local Wi-Fi network even when a VPN is the default network, joins must never block the UI thread, discovery can be restricted by access-point isolation, and process death cannot provide a graceful leave guarantee.

The pinned core's multiplayer receive timeout defaults to `25 ms`, and `RecvReplies()` is a synchronous core-facing wait. When application host and DS host differ, a pure star command/reply takes four network legs plus Android, worker, and emulator scheduling. The protocol exchange lifetime cannot extend the core's current wait, so physical latency feasibility is an architecture gate rather than release-end qualification.

## Goals / Non-Goals

**Goals:**

- Establish with physical measurements whether an application-host star can support the initially qualified two-to-four-device range within the core's receive deadline, then commit to the measured data topology and capacity.
- Let a running ROM or firmware session asynchronously host, discover, directly join, inspect, lock, unlock, leave, and diagnose a LAN session from the emulator pause flow.
- Keep the application session host independent from the emulated DS command host.
- Specify protocol v2 at the byte level before codec implementation.
- Preserve exact core frame limits and the AID-0 zero-length default-reply behavior while keeping DS data latency-oriented under loss.
- Give one native worker exclusive ownership of ENet and all native sockets.
- Pass only bounded copied values across worker, core, JNI, and UI boundaries.
- Make backend switching safe while emulated Wi-Fi is already powered.
- Make time, address resolution, datagram delivery, and fault decisions deterministic in unit tests.
- Keep the parent and native-core forks maintainable against their respective upstream projects.

**Non-Goals:**

- Internet matchmaking, NAT traversal, relay servers, WAN play, IPv6, authentication, encryption, or hostile-network security.
- Host migration or automatic reconnection/reservation after a disconnect.
- Rollback, input synchronization, save-state synchronization, or deterministic emulation.
- Improving wireless behavior that is already inaccurate in the emulator core.
- Desktop UI integration or compatibility with LAN protocol v1.
- Changing `LocalMP` behavior.
- Keeping a session alive after Android kills the app process or after the emulator session is explicitly stopped.

## Decisions

### 1. Fork both the Android parent and native-core submodule

The project will create a fork of `rafaelvcaetano/melonDS-android` and a project-owned fork of its `melonDS-android-lib` submodule. The parent fork will pin the project-owned native-core fork. Each repository will keep its original project as an `upstream` remote and record the exact baseline above.

The ENet submodule remains pinned to the existing ENet 1.3.18 commit. Vendoring or replacing ENet would add maintenance cost without solving the lifecycle and protocol problems.

Alternative considered: keep native changes in the parent app repository. Rejected because the interfaces, LAN implementation, and core adapter live in `melonDS-android-lib`; carrying duplicate sources in the parent would obscure ownership and upstream merges.

### 2. Treat the application-host star as a gated candidate topology

The candidate topology has every client maintain one ENet connection to the application host. The application host validates, routes, and relays DS frames; clients do not connect directly to one another. This topology is approved only for the Phase 0 vertical slice until its real-core timing is measured.

Capacity has two separate normative values:

- `protocolMaximumPlayers = 16`: the wire format, player-ID width, membership codec, and protocol tests can represent sixteen members without redesign; and
- `qualifiedMaxPlayers = 4` initially: host configuration, admission, ENet peer allocation, discovery advertisements, UI choices, release enablement, and public support claims are limited to `2..qualifiedMaxPlayers`.

A physical matrix qualifies only the largest simultaneous device count it actually passes. Raising `qualifiedMaxPlayers` to 8 or 16 requires a corresponding eight- or sixteen-device capacity run under the selected topology, the same deadline/latency criteria, host fan-out and queue-pressure evidence, and an updated decision record. Logical loopback coverage at sixteen members does not raise the qualified value.

The application host is authoritative for:

- session identity and lock state;
- player ID, generation, and stream assignment;
- membership revision and snapshots;
- admission and rejection;
- exchange records used for reply routing; and
- orderly host shutdown.

The application host does not automatically become the emulated DS host. The current DS host is the endpoint that originated a particular command exchange.

Phase 0 implements only enough framing, selected-network transport, router, facade, and core adapter to carry real command/reply exchanges. It runs on two, three, and four physical Android devices with application host deliberately different from DS host. Instrumentation records:

- monotonic `SendCmd` entry through `RecvReplies` return;
- all four relay legs;
- core-to-worker and worker-to-core queue residence;
- ENet service delay;
- responder emulator scheduling delay;
- completion/deadline-miss counts; and
- p50, p95, and p99 distributions.

The clean/ordinary-Wi-Fi gate requires at least 10,000 exchanges per required device combination, at least 99.9% completion before the configured `25 ms` core deadline, and end-to-end p99 no greater than `20 ms`. Controlled contention and packet-loss runs are recorded separately; they characterize degradation and loss policy rather than being mixed into the clean-latency percentile.

Phase 0 declares the normal, install-time `android.permission.WAKE_LOCK` permission before using a `WifiLock` and evaluates [`WIFI_MODE_FULL_LOW_LATENCY`](https://developer.android.com/reference/android/net/wifi/WifiManager#WIFI_MODE_FULL_LOW_LATENCY) both enabled and disabled on API level 29 or newer. The activity-retained multiplayer controller owns acquisition and release, holds the lock only for a foreground/screen-on measurement run, and releases it on run completion, failure, lifecycle stop, or teardown. Unsupported devices or acquisition failure do not fail transport or invalidate the no-lock half of the experiment. Production retains the code path only if evidence shows a material tail-latency benefit and documents the mode's foreground/screen-on limitation plus battery/throughput trade-off. If Phase 0 rejects it, the code path is removed and retention of `WAKE_LOCK` is reconsidered before release.

If the gate passes, the star becomes the normative full-implementation data topology. If it fails, implementation stops after the spike and this package is amended to choose exactly one of:

1. require application host = DS host;
2. retain the star for control but use a direct DS-host data/reply path;
3. use a hybrid or mesh data topology; or
4. increase the core timeout only after title-compatibility evidence.

Full lobby, production UI, diagnostics, and release work cannot begin until a decision record contains:

```text
selectedTopology
qualifiedMaxPlayers
testedDeviceMatrix
latencyGateResult
knownDeviceOrNetworkRestrictions
```

The initial record cannot set `qualifiedMaxPlayers` above 4 because Phase 0 has no larger physical capacity run.

### 3. Keep session state, endpoint activity, and locking independent

The externally visible session-state enum is:

`Idle`, `Discovering`, `StartingHost`, `Joining`, `Lobby`, `Stopping`, and `Failed`.

`Stopping` additionally carries a controller-owned `terminalTarget`, either `Idle` or `Failed(error)`. Cleanup completion is therefore independent from the eventual terminal result. `Failed` is retryable only because entry into it proves that no old worker remains, that worker termination/join has been confirmed where a worker existed, and that the facade has been restored. There is no inferred `Running` state. Separate snapshot fields carry:

- `endpointActive` for each member;
- `locked` for admission;
- `membershipRevision`; and
- `lastError`.

`MPInterface::Begin(instance)` marks only that local emulated Wi-Fi endpoint active. `End(instance)` marks it inactive. Neither changes `Lobby`, locks a session, or implies that gameplay started or ended.

The host explicitly locks and unlocks the session. A locked session rejects fresh joins with `SessionLocked`. Disconnects never reserve a slot. A disconnected device can only perform a fresh join, with a new generation and stream identity, after the host unlocks the session.

The state transitions are:

| Current | Command/event | Next | Required effect |
|---|---|---|---|
| Idle | Start discovery | Discovering | Open discovery socket and publish an initially empty list |
| Discovering | Stop/cancel | Idle | Close discovery socket and clear transient results |
| Discovering | Join selected/direct | Joining | Stop discovery receive, bind ENet to the selected Wi-Fi network, and start asynchronous join |
| Idle/Failed | Host | StartingHost | Clear the prior operation error and asynchronously create host/session |
| Idle/Failed | Join direct | Joining | Clear the prior operation error and asynchronously resolve/connect |
| StartingHost | Host ready | Lobby | Install LAN backend and publish local membership |
| Joining | Welcome accepted | Joining | Retain provisional assigned-client and host identities; wait for the initial authoritative membership snapshot |
| Joining | Matching initial membership accepted | Lobby | Verify ID-0 host identity and the assigned client identity/revision, then install LAN and publish membership |
| StartingHost/Joining | Cancel | `Stopping(Idle)` | Cancel pending work and begin bounded cleanup |
| StartingHost/Joining | Timeout/rejection/failure with resources to clean | `Stopping(Failed(error))` | Retain the typed terminal error and begin worker-owned cleanup |
| StartingHost/Joining | Failure after confirmed absence/termination of worker | `Failed(error)` | Restore `LocalMP` and retain a typed error |
| Lobby | Lock/unlock | Lobby | Host changes lock state and publishes a newer snapshot |
| Lobby | Local leave/host stop | `Stopping(Idle)` | Send accepted critical control, stop accepting new data, and clean up |
| Lobby | Host lost/network changed/protocol failure | `Stopping(Failed(error))` | Pause emulation, begin worker-owned cleanup, and retain the intended terminal error |
| Stopping | Worker stopped, joined, facade restored | terminal target | Release Android network/Wi-Fi resources and clear membership before entering `Idle` or `Failed(error)` |
| Stopping | 2000-ms SLO breached | `Stopping(Failed(ShutdownTimeout))` | Publish `ShutdownTimeout`, keep all new operations disabled, and continue awaiting worker-owned completion |
| Failed | Dismiss | Idle | Clear the visible error |
| Failed | Host/join/discover retry | corresponding state | Clear the prior operation error and start the new operation |
| Any non-Idle | Emulator session stops | `Stopping(Idle)`, then Idle | Perform bounded best-effort leave and restore `LocalMP` after cleanup |
| Any | Confirmed unexpected worker termination | `Failed(WorkerFailed)` | Join/confirm termination, restore the facade, then preserve the failure until dismissed or retried |

Commands not listed for a state return `InvalidState` without changing state. Host, join, and discovery are always invalid during `Stopping`, including after the two-second SLO. `Failed` remains observable until the user dismisses it or starts a new operation, and by definition never coexists with a live prior worker.

A fatal LAN failure never silently resumes emulation on `LocalMP`. Restoration is a memory-safe backend fallback, not semantic continuation of the multiplayer game. The emulator remains paused until the user acknowledges the modal failure and explicitly chooses whether to continue isolated, reset, or exit.

### 4. Make `MPInterface` a stable synchronized facade

`MPInterface::Get()` will return a process-wide facade whose address does not change. The facade independently retains:

- current backend type;
- the active-instance bit mask;
- a gate that prevents new calls during a switch;
- the in-flight call count; and
- the core receive timeout.

All core calls enter through the gate and hold an in-flight guard for the duration of the backend call. A backend switch is scheduled on the emulator control path and performs:

1. close the gate to new multiplayer calls;
2. wait for all in-flight calls to leave;
3. call `End` on the old backend for every active instance;
4. request worker stop, wait for worker-owned cleanup completion, and then join it;
5. install the new backend behind the facade;
6. copy the facade receive timeout into the new backend;
7. call `Begin` on the new backend for every active instance; and
8. reopen the gate.

`Begin` and `End` update the facade mask even while the dummy or local backend is installed. Calls arriving while the gate is closed wait for the bounded switch rather than dereferencing a destroyed object.

`LocalMP → LAN → LocalMP` switching while endpoint zero is already active is a required stress test. A failed host/join and every completed leave restore `LocalMP`.

Alternative considered: share ownership of replaceable polymorphic objects. Rejected because it still permits calls against a retired backend and does not solve active-instance replay.

### 5. Place ownership at the native worker and retained emulator runtime

Native components are:

- `MultiplayerSessionManager`: state machine, command admission, immutable snapshots, and backend-switch coordination.
- `ProtocolCodec`: pure checked encode/decode with no ENet types in its interface.
- `SessionRouter`: host membership, identity validation, sequence windows, and exchange routing.
- `EnetWorker`: the only owner of ENet hosts, peers, packets, discovery sockets, resolver-result admission, and network binding.
- `LanCoreAdapter`: bounded copied queues implementing the `MPInterface` calls.
- `MultiplayerFacade`: stable `MPInterface` entry point.

Kotlin components are:

- an activity-retained `MultiplayerRepository` owned alongside `EmulatorSession`;
- `MultiplayerViewModel`/state integrated with `EmulatorViewModel`;
- a Compose multiplayer overlay launched from both ROM and firmware pause menus; and
- an Android local-network binder that selects a Wi-Fi `Network`, passes its network handle to native code, and observes its loss.

Commands enter native code through non-blocking JNI methods and return immediate typed admission results. Multiplayer invalidation is not added to the emulator manager's unbounded `MutableSharedFlow`. Native maintains one atomic dirty bit and writes one pipe event only on a clean-to-dirty transition. Kotlin fetches one immutable snapshot, acknowledges the observed generation, and repeats if native became dirty during the fetch. The repository exposes the result through a conflated `StateFlow`. Native code never retains an `Activity`, `ViewModel`, Java object, `JNIEnv*`, or UI callback.

The session survives activity/configuration recreation while the activity-retained emulator runtime survives. `Activity.onPause` does not imply endpoint `End` and does not tear down LAN. Explicit emulator stop, runtime cleanup, or process death ends the session. The feature does not introduce a foreground service.

### 6. Bind sockets to the actual local Wi-Fi network

The app adds the normal `ACCESS_NETWORK_STATE` permission and uses `ConnectivityManager` to select an IPv4 `TRANSPORT_WIFI` network. It does not require `NET_CAPABILITY_VALIDATED`; a LAN without Internet is valid. Kotlin passes [`Network.getNetworkHandle()`](https://developer.android.com/reference/android/net/Network#getNetworkHandle()) to native code. Process-wide network binding is forbidden.

Pinned ENet creates and binds a host socket inside `enet_host_create(address, ...)`, so the application uses this exact sequence:

1. call `enet_host_create(nullptr, peer_count, 2, 0, 0)` so ENet creates a nonblocking UDP socket without binding it, using `peer_count = qualifiedMaxPlayers - 1` for an application host and exactly `1` for a client;
2. set `host->maximumPacketSize = 4096` and `host->maximumWaitingData = 65536` before admitting traffic;
3. call [`android_setsocknetwork(network_handle, host->socket)`](https://developer.android.com/ndk/reference/group/networking#android_setsocknetwork) and fail with `NetworkBindFailed` on error;
4. for an application host, call `enet_socket_bind()` with `ENET_HOST_ANY` and the selected session port, after the descriptor has been attached to the selected Android network, and map `EADDRINUSE`/equivalent to `PortUnavailable`;
5. call `enet_socket_get_address(host->socket, &host->address)` and verify the selected port; a wildcard address is valid, so the host's displayed direct-join IPv4 address comes from the selected network's retained `LinkProperties`, never from the wildcard-bound socket, while discovery clients continue to trust the received datagram source address; and
6. for a client, call `enet_host_connect()` only after steps 1 through 3.

Discovery sockets follow the same create → `android_setsocknetwork` → bind/send ordering. A pinned integration test verifies inbound hosting, outgoing connection, VPN-default routing, port collision, and cleanup through this nonstandard-but-public ENet lifecycle.

Hostname resolution uses the blocking [`Network.getAllByName()`](https://developer.android.com/reference/android/net/Network#getAllByName(java.lang.String)) on a dedicated bounded resolver executor and passes only resolved IPv4 bytes to native. Ordinary `getaddrinfo()` and `enet_address_set_host()` are forbidden for Android joins because they can follow the default network/VPN. At most four generation-tagged resolver calls may execute or await execution. Cancellation is logical rather than guaranteed interruption of DNS: cancel/teardown invalidates the generation immediately, the underlying blocking call may continue until Android returns, and any late result is ignored without touching a destroyed or newer native session. Capacity remains charged until that call actually returns. A result with no IPv4 address produces `HostNameResolvedNoIPv4`.

The v2 discovery transport is IPv4 UDP broadcast:

- discovery port: `7063`;
- default session port: `7064`;
- configurable session-port range: `1024..65535`, excluding `7063`;
- destination: `255.255.255.255:7063` on the selected Wi-Fi network;
- advertisement interval: `1000 ms`;
- expiry: `5000 ms`; and
- deduplication key: `(session_id, datagram_source_ipv4, session_port)`.

The source IPv4 address always comes from `recvfrom`; no advertised IP address is trusted. A discovery send/listen failure is nonfatal for a host: the lobby reports “direct connect only” and continues accepting direct joins. A joining client can enter an IPv4 address or resolvable local hostname when local-network permission is granted.

No location permission, Wi-Fi scan result, SSID, BSSID, or multicast lock is required by this broadcast design. [Android local-network permission](https://developer.android.com/privacy-and-security/local-network-permission) is versioned:

- **Target SDK 36 or lower:** do not declare or request `ACCESS_LOCAL_NETWORK`; `INTERNET` supplies ordinary implicit LAN access. Test Android 16 with `RESTRICT_LOCAL_NETWORK` both disabled and enabled. Under the compatibility restriction, document and handle the temporary `NEARBY_WIFI_DEVICES` grant behavior and map blocked UDP (`EPERM`) to `LocalNetworkPermissionDenied`.
- **Target SDK 37 or higher:** declare and request `ACCESS_LOCAL_NETWORK` after an in-context rationale and before discovery, hosting, or direct join. Denial or later revocation disables all three operations and produces `LocalNetworkPermissionDenied`; direct join is not an exception.

The selected-network observer retains the initial `Network` handle, interface name, IPv4 address/prefix, and route set from ordered `onLinkPropertiesChanged` callbacks. `onBlockedStatusChanged()` is registered/used only on API level 29 or newer. API levels 24–28 rely on permission checks, `onLost`, ordered capability/link-property callbacks, and mapped socket errors. Loss, replacement, blocked status where available, loss of IPv4, or a material interface/address/prefix/route change fails the session with `NetworkLost`, `NetworkChanged`, or `LocalNetworkPermissionDenied`; v2 does not migrate a live ENet host. Client isolation and router broadcast filtering remain discovery/connectivity diagnostics with direct join as fallback only when permission allows it.

The total monotonic deadline from acceptance of a join command through address resolution, ENet connect, hello, and welcome is `5000 ms`. ENet peers use a `500 ms` ping interval and timeout parameters `(limit=4, minimum=1000 ms, maximum=5000 ms)`. These are validated v2 constants, not preferences.

### 7. Define bounded queues and overload behavior

No queue stores an `ENetPacket*` or other transport-owned object. Decode occurs on the worker; accepted messages are copied into value types before crossing a boundary.

Initial validated constants are:

| Queue | Entry bound | Byte bound | Full behavior |
|---|---:|---:|---|
| Local UI/session commands | 64, with one reserved `Leave` slot and one reserved `Cancel` slot | 64 KiB | Ordinary command returns `QueueFull`; critical commands use their named slot; `Stop` uses an out-of-band atomic wake path |
| Remote control awaiting session manager | 64 per peer | 64 KiB per peer | Never evict; disconnect the peer with `ControlOverflow` |
| Coalesced membership snapshot | 1 | 4 KiB | Replace only with a newer revision |
| Core-to-worker DS frames | 256 | 512 KiB | Reject newest regular frame; command/reply/ack may evict the oldest regular frame, otherwise reject newest and emit a diagnostic |
| Worker-to-core DS frames | 256 | 512 KiB | Same class-aware policy as core-to-worker |
| UI diagnostics | 128 | 32 KiB | Drop oldest diagnostic and increment a dropped-diagnostic counter |

Critical lifecycle messages (`ClientHello`, `ServerWelcome`, `JoinRejected`, `Leave`, `HostShutdown`, local stop acknowledgement, peer disconnect, and protocol failure) are never silently evicted. Membership changes use coalescible full snapshots, not an essential chain of deltas.

ENet memory is bounded before admission:

- complete application packet: `4096` bytes through `ENetHost.maximumPacketSize`;
- reassembly/waiting data: `65536` bytes per peer through `maximumWaitingData`;
- reliable outgoing data: `65536` bytes per peer and `524288` bytes per host;
- the reliable charge is retained from ENet admission until the packet free callback proves ENet released it; and
- reliable fan-out uses per-peer packet ownership so charges can be released exactly.

Auxiliary collections are also bounded:

| Collection | Bound | Overflow/expiry |
|---|---:|---|
| Live exchange records | 16 | Reject newest command with `RouterOverload` |
| Discovery entries | 64 | Remove expired first, otherwise least recently seen |
| Connected but not welcomed peers | `qualifiedMaxPlayers - 1` (initially 3; protocol hard ceiling 15) | Reject additional ENet peers |
| Per-source hello/rate-limit records | 64 | Remove expired first, otherwise least recently seen nonblocked source |
| Outstanding resolver requests | 4 | Return `QueueFull` |
| Sequence-window records | 96 | Purge departed/stale identities; reject a new record if still full |
| Peer diagnostic records | `qualifiedMaxPlayers` (initially 4; protocol hard ceiling 16) | Coalesce counters into the owning live member |

A connected peer must send a valid `ClientHello` within `1500 ms` of the ENet connect event or be disconnected with local diagnostic `PeerHelloTimeout`. A stream ID need be unique only among live identities; generation plus the new stream protects slot reuse without an unbounded issued-ID history.

Once `Leave` or `Cancel` is accepted into its named reserved slot it is eventually observed unless the process terminates. `Stop` is idempotent, sets an atomic flag, and wakes the worker through a descriptor/condition path even if ordinary command capacity is exhausted.

All worker sockets are nonblocking; `enet_host_service()` uses a timeout no greater than `5 ms`; no synchronous DNS operation runs on the worker. Only the worker resets peers and destroys ENet/sockets, then signals cleanup completion. The controller joins only after that signal. Two seconds is the teardown SLO, not authority for cross-thread destruction. If breached, the facade remains gated, emulation stays paused, runtime/native-library cleanup is deferred, the public state remains `Stopping` with visible `ShutdownTimeout`, and the controller continues waiting asynchronously for worker completion. Only after cleanup, join, and facade restoration does the state become `Failed(ShutdownTimeout)`. `MelonEmulator.stopEmulation()` must not destroy state reachable by that worker.

Queued DS frames older than `100 ms` are stale and dropped before send/delivery. Exchange expiry has its own rule below. Queue constants are internal in v2 rather than user preferences.

### 8. Use deterministic seams for tests

The runtime depends on injected interfaces for:

- monotonic time;
- hostname/address resolution;
- socket/network binding;
- datagram ingress and egress; and
- random loss, delay, jitter, duplication, and reorder decisions in tests.

A fake clock drives hello/join timeout, discovery expiry, queue age, exchange expiry, sequence aging, and shutdown deadlines. A deterministic datagram shim or UDP proxy drives fault-policy tests. Real ENet loopback tests remain integration tests and do not substitute for deterministic loss tests.

### 9. Protocol-v2 common envelope

All integers are unsigned little-endian. Raw structures, compiler packing, pointer-sized fields, native enums, and native endianness are forbidden.

Every discovery or ENet application packet starts with this 32-byte envelope:

| Offset | Width | Field | Rule |
|---:|---:|---|---|
| 0 | 4 | `magic` | Raw ASCII `MLMP` (`4D 4C 4D 50`) |
| 4 | 1 | `framing_version` | `1` |
| 5 | 1 | `protocol_major` | `2` for this protocol |
| 6 | 1 | `protocol_minor` | `0` initially |
| 7 | 1 | `message_type` | Value from the message table |
| 8 | 2 | `flags` | Must be zero in v2 |
| 10 | 2 | `header_length` | Must be `32` |
| 12 | 2 | `payload_length` | Exact bytes after the envelope |
| 14 | 2 | `reserved` | Must be zero |
| 16 | 8 | `session_id` | Authoritative nonzero ID except the direct-join sentinel |
| 24 | 4 | `sequence` | Sender sequence; zero only where stated |
| 28 | 4 | `stream_id` | Assigned nonzero stream after welcome; zero during bootstrap/discovery |

The total datagram/packet length must equal `32 + payload_length`; trailing bytes are rejected. The complete application packet maximum is `4096` bytes; the envelope itself is always `32` bytes. A declared length is checked before allocation or copy.

The framing-version-1 envelope and `JoinRejected` bootstrap payload are stable across future protocol majors that adopt this bootstrap. A receiver validates magic, framing version, header size, total size, reserved bits, and message type before interpreting a major-specific payload. A host that receives a recognizable framing-v1 `ClientHello` for another protocol major sends the stable `ProtocolMajorMismatch` rejection. A client accepts that rejection for diagnostic purposes even though its envelope advertises the server's major.

Legacy protocol v1 does not share this envelope and is not guaranteed a rejection exchange. A v2 discovery listener may recognize the fixed legacy `LAND` advertisement magic and label it incompatible without parsing native-layout fields. Direct-connect bytes or timeout that do not form framing v1 produce `LegacyOrUnknownProtocol`, not a promise of `ProtocolMajorMismatch`.

Message types are:

| Value | Message |
|---:|---|
| `0x01` | `ClientHello` |
| `0x02` | `ServerWelcome` |
| `0x03` | `JoinRejected` |
| `0x04` | `MembershipSnapshot` |
| `0x05` | `EndpointState` |
| `0x06` | `Leave` |
| `0x07` | `HostShutdown` |
| `0x20` | `DSFrame` |
| `0x21` | `ChannelBarrier` |
| `0x40` | `DiscoveryAdvertisement` |

Unknown message types, nonzero envelope flags/reserved fields, unsupported trailing fields, and messages on the wrong transport/channel are malformed.

### 10. Compatibility, strings, and sequence arithmetic

`core_network_epoch` is initially `1`. It changes only when the maintained core/adapter contract becomes network-incompatible. Build identifiers are diagnostics and never cause rejection by themselves.

Feature bits are:

| Bit | Meaning |
|---:|---|
| 0 | player generation |
| 1 | exchange-correlated replies |
| 2 | explicit session lock |
| 3 | explicit unreliable fragmentation |

The v2 baseline has `supported_features = required_features = 0x000000000000000F`. A join is rejected if either side's required bits are absent from the other side's supported bits. Unknown optional supported bits are ignored. Minor versions interoperate when epoch and negotiated required features are compatible.

Player names are valid shortest-form UTF-8 of `1..31` bytes. Session names are valid shortest-form UTF-8 of `1..63` bytes. They cannot contain U+0000, U+0001..U+001F, U+007F..U+009F, U+202A..U+202E, or U+2066..U+2069. Build identifiers are `0..64` bytes in the printable ASCII range U+0020..U+007E. Lengths are byte counts.

Every assigned stream owns six independent sequence generators: control, regular, command, reply, ack, and channel barrier. Each generator starts at `1`, increments modulo `2^32`, and skips zero. `candidate` is newer than `last` exactly when `int32(candidate - last) > 0`. A receiver accepts the first nonzero sequence for a class, then accepts only a newer sequence under that comparison. Sequence windows are keyed by `(session_id, player_id, player_generation, stream_id, message class)` and reset for a new generation/stream. Relaying preserves the logical origin's sequence and class; `reply_to_command_sequence` always names the origin's command-class sequence. Discovery advertisement counters use the same arithmetic but are keyed by the discovery deduplication key.

### 11. Bootstrap and control payloads

`ClientHello` is sent reliably on ENet channel 0 after ENet connect:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | `core_network_epoch` |
| 4 | 1 | `player_name_length` |
| 5 | 1 | `build_id_length` |
| 6 | 2 | reserved, zero |
| 8 | 8 | `supported_features` |
| 16 | 8 | `required_features` |
| 24 | variable | player name, then build ID |

A discovered join puts the advertised session ID in the envelope. A direct join puts the all-zero session-ID sentinel in the envelope. `ClientHello` uses sequence `1` and stream `0`. The host rejects a nonzero expected ID that no longer names its current session with `SessionReplaced`.

`ServerWelcome` is reliable on channel 0 and carries the authoritative nonzero session ID:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 1 | `player_id` |
| 1 | 1 | configured `max_players` (`2..qualifiedMaxPlayers`) |
| 2 | 2 | `player_generation` |
| 4 | 4 | assigned nonzero `stream_id` |
| 8 | 1 | host player ID, exactly `0` |
| 9 | 1 | reserved, zero |
| 10 | 2 | host generation, exactly `1` in v2 |
| 12 | 4 | host nonzero `stream_id` |
| 16 | 4 | `membership_revision` |
| 20 | 4 | `core_network_epoch` |
| 24 | 8 | `supported_features` |
| 32 | 8 | `required_features` |
| 40 | 1 | `session_name_length` |
| 41 | 1 | `build_id_length` |
| 42 | 2 | reserved, zero |
| 44 | variable | session name, then host build ID |

`ServerWelcome` uses envelope sequence `1` and stream `0`; its payload establishes both the assigned client identity and the session's ID-0 application-host identity. `JoinRejected` also uses envelope sequence `1` and stream `0`, and its envelope carries the host's current nonzero session ID. The client accepts a bootstrap rejection even when that ID differs from a discovered expectation. After welcome, host-originated control uses the host member's nonzero stream, and client-originated control uses the client's assigned nonzero stream.

Accepting `ServerWelcome` does not yet enter `Lobby`. The client stays in `Joining`, records both identities provisionally, and waits within the existing join deadline for the first reliable `MembershipSnapshot`. That snapshot must contain ID 0 with the exact welcome host generation/stream, the assigned client identity from the welcome, and a revision equal to or newer than the welcome revision. Only then is minimum authoritative membership established and the client enters `Lobby`. Any disagreement is `ProtocolViolation` and enters `Stopping(Failed(ProtocolViolation))` until cleanup completes.

`JoinRejected` has a stable framing-v1 payload:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | rejection reason |
| 2 | 1 | server protocol major |
| 3 | 1 | server protocol minor |
| 4 | 4 | server core-network epoch |
| 8 | 8 | server required features |
| 16 | 1 | server build-ID length |
| 17 | 3 | reserved, zero |
| 20 | variable | server build ID |

Rejection codes are:

| Value | Reason |
|---:|---|
| 1 | `SessionFull` |
| 2 | `SessionLocked` |
| 3 | `ProtocolMajorMismatch` |
| 4 | `RequiredFeatureUnsupported` |
| 5 | `CoreEpochMismatch` |
| 6 | `SessionReplaced` |
| 7 | `InvalidHello` |
| 8 | `InvalidName` |
| 9 | `ServerStopping` |
| 10 | `RateLimited` |
| 11 | `InternalError` |

Local typed errors/diagnostics, which are not wire rejection codes, include `InvalidState`, `LocalNetworkPermissionDenied`, `NoLocalWifi`, `AddressInvalid`, `ResolveFailed`, `HostNameResolvedNoIPv4`, `NetworkBindFailed`, `PortUnavailable`, `ConnectTimeout`, `HostUnreachable`, `DiscoveryUnavailable`, `LegacyOrUnknownProtocol`, `PeerHelloTimeout`, `NetworkLost`, `NetworkChanged`, `QueueFull`, `RouterOverload`, `RelayQueueDrop`, `Stopping`, `ProtocolViolation`, `ControlOverflow`, `ShutdownTimeout`, and `WorkerFailed`.

`MembershipSnapshot` is reliable on channel 0:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | `membership_revision` |
| 4 | 1 | `locked` (`0` or `1`) |
| 5 | 1 | entry count (`1..16`) |
| 6 | 2 | reserved, zero |
| 8 | variable | entries sorted by player ID |

Each entry is:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | entry length, exactly `16 + name_length` |
| 2 | 1 | player ID (`0..15`) |
| 3 | 1 | endpoint active (`0` or `1`) |
| 4 | 2 | player generation |
| 6 | 2 | ping in milliseconds, `0xFFFF` if unknown |
| 8 | 4 | stream ID |
| 12 | 1 | name length |
| 13 | 3 | reserved, zero |
| 16 | variable | player name |

The host increments a nonzero 32-bit membership revision for every accepted membership, endpoint-active, or lock change, skipping zero on wrap. Receivers apply only a newer full snapshot. No `PlayerLeft` delta exists.

`EndpointState` is an 8-byte client-to-host reliable message: player ID at 0, active flag at 1, generation at 2, and stream ID at 4. The host validates the sending peer identity, updates membership, and publishes a full snapshot.

`Leave` is an 8-byte reliable message: player ID at 0, reason at 1, generation at 2, and stream ID at 4. `HostShutdown` is the following exact four-byte reliable payload:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | reason, unsigned little-endian |
| 2 | 2 | reserved, must be zero |

Both are session-layer messages and never reach the emulated Wi-Fi core. A `HostShutdown` with an unknown 16-bit reason or nonzero reserved field is malformed.

Leave reasons are user request `0`, emulator stop `1`, protocol failure `2`, and network change `3`. Host-shutdown reasons are user request `0`, emulator stop `1`, network loss/change `2`, and internal failure `3`. Unknown reasons are malformed.

`DiscoveryAdvertisement` uses the common envelope with nonzero session ID, stream zero, and an advertisement counter in `sequence`:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 2 | session port |
| 2 | 1 | current players |
| 3 | 1 | configured maximum players (`2..qualifiedMaxPlayers`; wire range `2..16`) |
| 4 | 1 | locked (`0` or `1`) |
| 5 | 1 | session-name length |
| 6 | 1 | build-ID length |
| 7 | 1 | reserved, zero |
| 8 | 4 | membership revision |
| 12 | 4 | core-network epoch |
| 16 | 8 | supported features |
| 24 | 8 | required features |
| 32 | variable | session name, then build ID |

### 12. Player identity and reuse

At application-session creation, before accepting any ENet peer, the host creates its local member identity as:

```text
player_id       = 0
generation      = 1
stream_id       = cryptographically random nonzero uint32
transport_peer  = none/local
```

The stream ID is redrawn on collision with any live identity. This identity is included in initial membership revision 1 and in every later full snapshot. Accepting the first client increments the revision before sending that client's welcome and matching initial snapshot. The host then assigns accepted clients IDs `1..15`, subject to the configured maximum not exceeding `qualifiedMaxPlayers`. The protocol does not use ID 0 to identify the DS host.

Each player slot has a 16-bit generation. The first assignment uses generation 1. Reuse increments the generation and skips zero. If increment would wrap from `0xFFFF` to zero, that slot is retired for the rest of the session. Every accepted connection also gets a random nonzero 32-bit stream ID that is unique among live identities in that session; a live collision is redrawn.

The routed identity is:

`(session_id, player_id, player_generation, stream_id, sequence)`.

Packets with a stale generation, stale stream, mismatched ENet peer, or unacceptable sequence are dropped before routing. Session IDs are random nonzero 64-bit values generated from the platform secure random source; they prevent accidental collision but are not authentication tokens.

### 13. DS frame schema and exchange routing

`DSFrame` uses this payload:

| Offset | Width | Field |
|---:|---:|---|
| 0 | 1 | sender player ID |
| 1 | 1 | destination player ID, or `0xFF` for broadcast |
| 2 | 1 | class: regular `0`, command `1`, reply `2`, ack `3` |
| 3 | 1 | association ID (AID) |
| 4 | 2 | sender generation |
| 6 | 2 | destination generation, zero for broadcast |
| 8 | 2 | frame length |
| 10 | 2 | reserved, zero |
| 12 | 4 | destination stream ID, zero for broadcast |
| 16 | 8 | emulated timestamp |
| 24 | 4 | `reply_to_command_sequence`, zero when not correlated |
| 28 | variable | exact frame bytes |

The envelope stream/sequence plus payload sender fields identify the **logical DS origin**, which is distinct from the ENet **transport peer**:

- when the application host receives a DS frame from a client, the logical origin must exactly equal the identity assigned to that ENet peer;
- when a client receives a relayed DS frame, its transport peer must be the application host, while the logical origin may be any identity the host authorized when it relayed the frame;
- clients trust that host authorization even if the corresponding reliable membership snapshot has not yet crossed channel 0, so cross-channel ordering does not delay core delivery;
- host-local delivery preserves the original logical origin;
- the host preserves logical identity, frame bytes, emulated timestamp, class sequence, and command correlation across relay; and
- control messages are never relayed as another identity and always remain bound to their ENet transport peer.

This trust rule is safe only within the non-hostile-LAN threat model; it is not cryptographic authorization.

Regular, command, and ack frames must be core-deliverable internal Wi-Fi values of `36..0x948` bytes. Payload replies must be `36..1024` bytes. For every payload frame, the little-endian internal frame length at bytes 10..11 must equal `total_length - 12`. The only legal zero-length DS value is an AID-0 default reply; zero-length regular, command, ack, or payload-bearing reply is rejected and is never claimed to be observable by `Wifi::CheckRX()`. Oversize, undersize, or internally length-mismatched outbound frames are rejected before allocation; their inbound equivalents are malformed.

Regular and command frames are broadcast with destination `0xFF`, zero destination generation/stream, AID zero, and zero correlation except that each command's envelope sequence becomes its exchange identity.

When the application host accepts a command, it creates an exchange record keyed by:

`(session_id, sender_id, sender_generation, sender_stream_id, command_sequence)`.

The record contains the exact DS-host destination identity, the eligible responder identities at command acceptance, an empty responder/AID set, the command's emulated timestamp, host monotonic `accepted_at`, and `expires_at`. An eligible responder is an accepted current member whose endpoint is active, excluding the logical command origin. The monotonic lifetime is `min(500 ms, max(100 ms, 4 × core_receive_timeout))`.

A reply contains the command sequence in `reply_to_command_sequence` and the exact command sender identity as its destination. Payload-bearing replies require AID `1..15`. AID 0 is valid only for a zero-length default reply. A payload-bearing AID 0, AID above 15, absent/expired exchange, wrong destination identity, sender not in the expected responder set, or arrival after monotonic `expires_at` is rejected. Emulated timestamps from different devices are not assumed synchronized. The transport retains the timestamp for exchange diagnostics, but `RecvReplies()` has no timestamp output; cross-device timestamp comparison is intentionally replaced by explicit exchange correlation and the host monotonic window.

Only the first reply from a responder for an exchange is accepted. For payload replies, the first accepted reply for a given AID occupies that core slot; later collisions are dropped deterministically. A zero-length AID-0 default reply records that responder without occupying a payload slot.

An ack is broadcast, carries zero destination identity and AID, and references a nonzero command sequence. The host removes the exchange after ack, expiry, or departure/generation change of its DS-host identity. Thus a delayed reply for one DS host cannot be routed to a later DS host.

If an expected responder leaves or becomes endpoint-inactive after command acceptance, the host removes it from the expected set; an endpoint that becomes active later is not added. Failure to enqueue a relay locally is `RelayQueueDrop`, removes that destination from the expected set, and increments a per-destination diagnostic. Successful enqueue followed by network loss is ordinary unreliable transport loss, so the destination remains expected until departure, inactivity, or expiry.

#### Core-adapter retained contexts

The unchanged core API requires explicit per-instance retained context:

- **Inbound command context:** immediately before `RecvPacket()` or `RecvHostPacket()` returns a command, the adapter stores logical sender ID/generation/stream, command-class sequence, and emulated timestamp. It does not deliver another command to that instance until the first accepted `SendReply()` consumes the context or its exchange window expires. A reply with no live inbound context returns `0`.
- **Outbound command context:** an accepted `SendCmd()` assigns the command-class sequence and stores its exchange identity/timestamp. There is at most one outstanding outbound command per local instance. A new accepted `SendCmd()` deterministically marks the prior context superseded, discards its pending replies, records a diagnostic, and installs the new context, matching the current core's reset-on-command behavior.
- **Logical last-host context:** delivering a command records that logical sender independently of the application-host connection. `RecvHostPacket()` returns `-1` if that identity leaves, changes generation/stream, becomes endpoint-inactive, or the LAN session stops/fails, even when the application host remains connected.

`RecvReplies()` zero-fills all fifteen 1-KiB AID slots before waiting and aggregates only replies correlated to the current outbound context. Completion is deliberately asymmetric:

- when the application host's local endpoint is the logical DS host, the adapter may use the colocated router's authoritative exchange record and return when `(receivedAidMask & requestedAidMask) == requestedAidMask`, every authoritative expected responder has replied/departed/become ineligible, the facade receive timeout expires, or the session stops/fails; but
- when a client's local endpoint is the logical DS host, it returns only when `(receivedAidMask & requestedAidMask) == requestedAidMask`, the facade receive timeout expires, or the session stops/fails. It never infers responder exhaustion from its possibly stale membership snapshot.

No latency-sensitive `ExchangeComplete` control message exists in v2. The existing cross-device `reply_timestamp < current_timestamp - 32` test is intentionally removed because device emulated clocks are not synchronized; exchange identity replaces it. `SendAck()` references and closes the current outbound context after successful queue admission.

On stop or network failure, all blocking adapter waits are woken: `RecvPacket()` returns `0`, `RecvHostPacket()` returns `-1`, and `RecvReplies()` returns `0` after zero-fill. A nonzero `SendPacket`, `SendCmd`, `SendReply`, or `SendAck` returns the submitted length only after queue admission and returns `0` for invalid length/context, full/stopping queue, or shutdown. A legal zero-length AID-0 default reply preserves the existing API ambiguity and returns `0` whether admitted or rejected; admission/drop diagnostics are internal because the pinned core does not distinguish those outcomes.

### 14. ENet channel and flag policy

The ENet host is created with exactly two channels:

| Traffic | Channel | ENet flags | Behavior |
|---|---:|---|---|
| Bootstrap, session control, membership | 0 | `ENET_PACKET_FLAG_RELIABLE` | Reliable and ordered |
| DS frames | 1 | `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT` only | Sequenced, non-retransmitted fragments; never `RELIABLE` or `UNSEQUENCED` |
| Channel barrier | 1 | `ENET_PACKET_FLAG_RELIABLE` | Non-core rollover marker |
| Discovery advertisement | UDP socket | none | One bounded datagram |

ENet 1.3.18 promotes an oversized unreliable packet to reliable fragmentation if `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT` is absent or its channel's 16-bit unreliable sequence is exhausted. The worker therefore sends a reliable zero-payload `ChannelBarrier` before the channel reaches 60,000 unreliable packets. Successful ENet admission advances the reliable sequence and resets the peer's unreliable sequence before DS sends resume. A barrier is consumed by the session layer and never forwarded to the core. It carries the current session ID and a normal nonzero barrier-class application sequence.

Above-MTU regular, command, reply, and ack frames are explicitly tested under loss to prove that missing fragments drop the DS frame rather than causing payload retransmission. A lost reliable rollover barrier temporarily stalls later channel-1 unreliable delivery until ENet retransmits the barrier; this rare bounded synchronization stall is accepted. Long-run tests cross multiple barriers and prove that no DS payload is promoted or retransmitted while measuring the barrier stall.

The host accepts at most eight `ClientHello` attempts from one source IPv4 address in any rolling ten-second interval. Further attempts receive `RateLimited` when a reply can be sent and are ignored for 30 seconds. After bootstrap, three nonfatal malformed application messages from one peer in a rolling ten-second interval cause disconnect. Oversize envelopes, wrong post-welcome session ID, forged identity, and control-budget overflow cause immediate disconnect. Malformed discovery datagrams are always dropped without peer/session mutation.

### 15. UI and persisted configuration

Both ROM and firmware pause menus gain `Multiplayer`. Selecting it pauses emulation and opens a Compose overlay with:

- host or join role;
- discovery results and refresh;
- direct IPv4/hostname and port entry;
- player name;
- session name and maximum players for a host;
- lobby members with endpoint-active and latency diagnostics;
- host lock/unlock;
- direct-connect address/port;
- leave/cancel/retry; and
- concise typed error details.

All host/join operations are asynchronous. Dismissing an in-progress setup cancels it and restores `LocalMP`. Entering a lobby does not automatically resume emulation; the user explicitly returns to play. Closing/recreating the activity rehydrates the overlay from the retained immutable snapshot rather than restarting the native operation.

Persist only:

- player name;
- session name;
- maximum players;
- session port;
- discovery enabled; and
- diagnostics enabled.

Protocol limits, queue sizes, sequence windows, expiry intervals, and ENet flags are validated constants, not preferences.

## Risks / Trade-offs

- **Wi-Fi access-point isolation or broadcast filtering prevents discovery** → Keep direct join first-class, identify discovery-only failure separately, and show the host's selected IPv4 endpoint.
- **Android process death cannot send leave** → ENet timeout removes the peer; the host publishes a new full membership snapshot. No reconnection identity is promised.
- **A two-repository fork increases maintenance** → Pin exact commits, keep upstream remotes, isolate protocol/router code, and document the parent/submodule update sequence.
- **Four-leg star command/reply misses the 25-ms core deadline** → Validate it in Phase 0 before full implementation and select a pre-agreed fallback topology if p99 exceeds the gate.
- **Representable sixteen-member protocol is mistaken for qualified support** → Initially cap host/UI/advertisements/claims at four and raise `qualifiedMaxPlayers` only after a same-size physical capacity run updates the decision record.
- **Client DS host has stale membership while waiting for replies** → Never let a client adapter terminate on locally inferred responder exhaustion; use requested-mask completion, timeout, or session termination only.
- **ENet's internal 16-bit channel counters can alter delivery semantics** → Use the explicit data-channel barrier, accept its rare bounded synchronization stall, and test past rollover.
- **Reliable control or fragment reassembly consumes memory before codec validation** → Set ENet packet/waiting-data caps immediately, retain reliable budget charges through ENet release, bound every auxiliary table, and disconnect on overflow.
- **Worker teardown exceeds two seconds** → Keep the state in `Stopping`, disable retries, freeze/pause runtime, and defer destruction rather than cross-thread destroy ENet; enter retryable `Failed(ShutdownTimeout)` only after cleanup/join.
- **Android local-network permission changes with target SDK** → Implement and test separate <=36 and >=37 policies, including denial and revocation.
- **Pausing emulation during setup may cause a title to time out if setup begins mid-exchange** → Document that setup is safest before entering a game's multiplayer flow; the backend switch still safely handles already-active Wi-Fi.
- **Protocol v2 is intentionally incompatible with existing desktop LAN** → Provide a clear mismatch only for shared framing-v1 peers; report legacy/direct ambiguity as `LegacyOrUnknownProtocol` and do not provide fragile v1 fallback.
- **Retail-title qualification cannot be fully automated** → Keep automated tests content-free and define named, reproducible manual qualification workflows before release.

## Migration Plan

1. Create project-owned forks of the Android parent and native-core submodule; add their original repositories as `upstream`; pin and record the reviewed baselines.
2. Clone the Android parent fork into the implementation workspace, copy this OpenSpec change package into it, validate it there, and update `.gitmodules` to the project-owned native-core fork.
3. Land only the minimum framing, facade, adapter contexts, router relay, selected-network ENet binding/DNS, ENet memory caps, worker teardown, and timing instrumentation required by Phase 0 behind a spike-only build flag.
4. Declare `WAKE_LOCK`, then run the two/three/four-device real-core timing matrix, including application host ≠ DS host, OEM/API variation, contention/loss characterization, and API-29+ low-latency Wi-Fi mode A/B measurements.
5. Record `selectedTopology`, `qualifiedMaxPlayers` (at most 4 for this matrix), `testedDeviceMatrix`, `latencyGateResult`, and known device/network restrictions. If the star fails, amend the design/spec/tasks before further implementation; do not silently continue with another topology or timeout.
6. Only after a passing/approved topology decision, land the complete codec, deterministic suites, production router/session manager, Kotlin repository, settings, Compose overlay, JNI snapshot bridge, and lifecycle tests.
7. Enable the feature for debug/nightly builds, run sanitizer/fault/soak tests, then complete release physical qualification before enabling release builds.
8. Roll back by disabling the Android feature flag and pinning the parent to the prior native-core submodule commit. Saved multiplayer preferences are inert and safe to retain.

## Open Questions

- The Phase 0 content-free workflow must be named before spike measurements begin; later retail qualification titles must be named before the release-candidate task.
- The final data topology remains deliberately open until the Phase 0 p99 gate is recorded. Failure requires a planning amendment, not implementation discretion.
- The initially enabled capacity is four. Any request to advertise or enable eight or sixteen players requires a same-size physical qualification update rather than a wire-format-only change.
- The GitHub namespace and final fork URLs are operational choices resolved when the forks are created; they do not change the protocol or architecture.
