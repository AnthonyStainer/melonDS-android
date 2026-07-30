## ADDED Requirements

### Requirement: Android fork baselines
The implementation SHALL target a project-owned fork of `rafaelvcaetano/melonDS-android` at baseline `ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6` and a project-owned fork of its `melonDS-android-lib` submodule at baseline `431ab4bd0003c4356e25fce89640ae1004579b9b`. The parent fork SHALL pin the project-owned native-core fork, retain ENet 1.3.18 at `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`, and document upstream remotes.

#### Scenario: Implementation workspace is prepared
- **WHEN** implementation begins
- **THEN** the Android parent and native-core fork baselines and upstream remotes are verifiable
- **AND** the parent submodule URL points at the project-owned native-core fork

### Requirement: Physical timing feasibility gate
Before full lobby, production UI, or diagnostics implementation, a minimal real-core vertical slice SHALL run command/reply traffic on two, three, and four physical Android devices with application host deliberately different from DS host. Every required device combination SHALL record at least 10,000 clean exchanges, all relay-leg and queue timings, completion/deadline misses, and p50/p95/p99 `SendCmd`-entry-to-`RecvReplies`-return latency. The candidate star passes only with at least 99.9% completion before the 25-ms core deadline and p99 no greater than 20 ms. The resulting record SHALL contain `selectedTopology`, `qualifiedMaxPlayers`, `testedDeviceMatrix`, `latencyGateResult`, and `knownDeviceOrNetworkRestrictions`; this four-device matrix SHALL NOT set `qualifiedMaxPlayers` above 4.

#### Scenario: Candidate star passes
- **WHEN** every mandatory physical combination meets both clean-latency thresholds
- **THEN** the measurements and star topology decision are recorded
- **AND** the initially enabled capacity is recorded as no more than four players
- **AND** full implementation tasks may begin

#### Scenario: Candidate star fails
- **WHEN** any mandatory combination misses a threshold
- **THEN** full implementation remains blocked
- **AND** this change package is amended to require application host = DS host, direct DS-host data, a hybrid/mesh data path, or an evidence-backed core-timeout change

### Requirement: Evidence-driven low-latency Wi-Fi evaluation
Before creating any Phase 0 `WifiLock`, the manifest SHALL declare the normal `android.permission.WAKE_LOCK` permission. Phase 0 SHALL guard `WIFI_MODE_FULL_LOW_LATENCY` behind API level 29 and measure supported devices with the mode both enabled and disabled. The activity-retained multiplayer controller SHALL own acquisition/release and hold it only for a foreground/screen-on run. Unsupported mode or acquisition failure SHALL NOT fail transport. The production design SHALL retain the code path only if it materially improves tail latency and documents its foreground/screen-on limitation plus battery/throughput trade-offs; otherwise the code path SHALL be removed and continued need for `WAKE_LOCK` SHALL be reconsidered.

#### Scenario: Low-latency mode is evaluated
- **WHEN** the physical timing matrix runs on a device supporting the mode
- **THEN** paired measurements with and without the lock are recorded
- **AND** the lock is released after each foreground measurement session

#### Scenario: Low-latency lock cannot be acquired
- **WHEN** the API-29-or-newer device rejects or cannot acquire the experimental lock
- **THEN** the no-lock transport run remains valid
- **AND** the failure is recorded without failing session transport

### Requirement: Multiplayer pause-menu entry
Both ROM and firmware emulator pause menus SHALL expose a Multiplayer action. Selecting it SHALL pause emulation and open a host/join overlay without stopping the emulator session.

#### Scenario: ROM user opens multiplayer
- **WHEN** a user selects Multiplayer from a running ROM's pause menu
- **THEN** emulation is paused
- **AND** the multiplayer overlay shows host and join choices

#### Scenario: Firmware user opens multiplayer
- **WHEN** a user selects Multiplayer while booted into DS or DSi firmware
- **THEN** the same multiplayer host/join capability is available

### Requirement: Non-blocking Android operations
Discovery, hostname resolution, hosting, joining, cancellation, leaving, and worker shutdown SHALL execute asynchronously outside the Android main thread. JNI command calls SHALL return an immediate typed admission result, and operation completion SHALL arrive through immutable state.

#### Scenario: Join is slow
- **WHEN** a target host does not answer during the join timeout
- **THEN** the Android main thread remains responsive
- **AND** the operation ends with `ConnectTimeout` or `HostUnreachable`

#### Scenario: Local command queue is full
- **WHEN** the UI submits a noncritical command with no available command capacity
- **THEN** JNI returns `QueueFull`
- **AND** the UI remains synchronized with the last accepted snapshot

### Requirement: Explicit session state
The session state SHALL be one of `Idle`, `Discovering`, `StartingHost`, `Joining`, `Lobby`, `Stopping`, or `Failed`. `Stopping` SHALL retain a terminal target of `Idle` or `Failed(error)`. Endpoint activity, lock state, membership revision, and last error SHALL be separate snapshot fields. A state SHALL enter retryable `Failed` only after the prior worker is confirmed absent/stopped and joined, the facade is restored, and retained Android network/Wi-Fi resources are released. Commands invalid for the current state SHALL return `InvalidState` without changing state.

#### Scenario: Wi-Fi endpoint becomes active in a lobby
- **WHEN** the emulated Wi-Fi hardware calls `MPInterface::Begin`
- **THEN** the local member's endpoint-active field becomes true
- **AND** the session remains in `Lobby`
- **AND** the lock state does not change

#### Scenario: Failure remains diagnosable
- **WHEN** joining fails
- **THEN** the session first completes any required `Stopping(Failed(error))` cleanup
- **AND** only then enters `Failed`
- **AND** its typed error remains visible until the user dismisses it or starts another operation

#### Scenario: Shutdown exceeds two seconds
- **WHEN** a failing session's prior worker has not completed cleanup after 2000 ms
- **THEN** the public state remains `Stopping` with visible `ShutdownTimeout`
- **AND** host, join, and discovery actions remain disabled
- **AND** `Failed(ShutdownTimeout)` is entered only after worker stop/join and facade restoration

#### Scenario: Welcome precedes initial membership
- **WHEN** a joining client accepts `ServerWelcome`
- **THEN** it remains in `Joining` with provisional assigned-client and host identities
- **AND** it enters `Lobby` only after a matching authoritative initial membership snapshot

### Requirement: Explicit session admission lock
Only the application host SHALL change the session's explicit lock. A locked session SHALL reject new joins with `SessionLocked`. `MPInterface::Begin` and `End` SHALL NOT lock or unlock a session.

#### Scenario: Host locks the lobby
- **WHEN** the host locks a lobby containing connected members
- **THEN** the host publishes a newer membership snapshot with `locked=true`
- **AND** existing members remain connected

#### Scenario: New client targets a locked lobby
- **WHEN** a fresh client sends a valid hello to a locked session
- **THEN** the host rejects it with `SessionLocked`

### Requirement: Fresh join after disconnection
Protocol v2 SHALL NOT reserve player identity after a disconnect. A disconnected device SHALL perform a fresh join and receive a new stream identity and, when its player slot is reused, a newer player generation. A fresh join is accepted only while the session is unlocked and has capacity.

#### Scenario: Device rejoins an unlocked lobby
- **WHEN** a disconnected device submits a new valid hello after the host unlocks the session
- **THEN** it is processed as a new participant
- **AND** it cannot resume the prior generation/stream identity

### Requirement: Stable multiplayer facade
`MPInterface::Get()` SHALL return a stable process-wide facade. Backend switching SHALL quiesce new calls, drain in-flight calls, end every active instance on the old backend, stop and join the old worker, install the new backend, replay begin for every active instance, and then reopen calls.

#### Scenario: LAN is selected while emulated Wi-Fi is active
- **WHEN** instance zero has already called `Begin` and the backend switches from `LocalMP` to LAN
- **THEN** no core callback can access a destroyed backend
- **AND** LAN receives `Begin(0)` before core calls resume

#### Scenario: LAN setup fails
- **WHEN** hosting or joining fails after backend preparation starts
- **THEN** the system safely restores `LocalMP`
- **AND** every still-active instance is replayed onto `LocalMP`

### Requirement: Endpoint activity semantics
The facade SHALL retain an active-instance mask independently of the installed backend. `Begin(instance)` SHALL set the corresponding bit and notify the installed backend exactly once for that inactive-to-active transition; `End(instance)` SHALL clear it and notify exactly once for the active-to-inactive transition.

#### Scenario: Title power-cycles emulated Wi-Fi
- **WHEN** a title calls `End` and later `Begin` without leaving the application lobby
- **THEN** only endpoint activity changes are published
- **AND** membership and application session identity remain intact

### Requirement: Retained Android ownership
The multiplayer repository/session owner SHALL live with the activity-retained emulator runtime rather than an `Activity`. Activity recreation and `Activity.onPause` SHALL NOT implicitly leave or recreate a LAN session. Explicit emulator stop or runtime cleanup SHALL perform bounded best-effort leave and request worker-owned cleanup. Runtime/native-library state reachable by a live worker SHALL NOT be destroyed.

#### Scenario: Emulator activity is recreated
- **WHEN** Android recreates the emulator activity while the native session remains alive
- **THEN** the new UI instance renders the retained immutable snapshot
- **AND** no duplicate host, join, or ENet worker is created

#### Scenario: Emulator exits
- **WHEN** the emulator session is explicitly stopped
- **THEN** LAN begins its bounded stopping sequence
- **AND** the backend returns to `LocalMP` only after the worker signals cleanup completion

#### Scenario: Android kills the process
- **WHEN** Android terminates the application process
- **THEN** the protocol makes no graceful-leave guarantee
- **AND** peers remove the connection through ENet timeout and authoritative membership revision

### Requirement: Immutable Android snapshot bridge
Native code SHALL publish multiplayer-state invalidation with one atomic dirty bit and at most one outstanding pipe event. Kotlin SHALL fetch complete immutable snapshots through JNI, acknowledge an observed generation, repeat if dirtied concurrently, and expose state through a conflated `StateFlow`. Multiplayer invalidations SHALL NOT be buffered in an unbounded `SharedFlow`. Native code SHALL NOT retain an `Activity`, `ViewModel`, Java UI object, `JNIEnv*`, or UI callback.

#### Scenario: Membership changes rapidly
- **WHEN** multiple native membership changes occur before the UI consumes them
- **THEN** Kotlin can fetch the newest complete snapshot
- **AND** it never observes a partially mutated member list

### Requirement: Local Wi-Fi network selection
The Android layer SHALL select an IPv4 `TRANSPORT_WIFI` network without requiring validated Internet. It SHALL pass `Network.getNetworkHandle()` to native, bind every discovery and ENet descriptor with `android_setsocknetwork()` before bind/connect/send, and SHALL NOT bind the whole process. Hostname resolution SHALL use blocking selected-network `Network.getAllByName()` on an executor with at most four executing/queued calls and pass only resolved IPv4 bytes to native. Cancellation SHALL be logical: invalidate the request generation immediately, retain capacity until the blocking call returns, and discard late results without touching a destroyed or newer session.

#### Scenario: Wi-Fi has no Internet
- **WHEN** the device is connected to an unvalidated local Wi-Fi network with an IPv4 address
- **THEN** hosting, discovery, and direct join remain eligible

#### Scenario: VPN is the default network
- **WHEN** Android's default network is a VPN but a local IPv4 Wi-Fi network is available
- **THEN** multiplayer sockets bind to the selected Wi-Fi network

#### Scenario: Host name has no IPv4 result
- **WHEN** selected-network DNS returns only non-IPv4 addresses
- **THEN** joining fails with `HostNameResolvedNoIPv4`

### Requirement: Target-SDK-versioned local-network permission
The app SHALL declare normal permissions including `INTERNET`, `ACCESS_NETWORK_STATE`, and the install-time `WAKE_LOCK` permission while the Phase 0 low-latency experiment exists, and SHALL NOT require location, SSID, BSSID, scan results, or a multicast lock. For target SDK 36 or lower it SHALL NOT declare/request `ACCESS_LOCAL_NETWORK`, SHALL test Android 16 `RESTRICT_LOCAL_NETWORK`, and SHALL map compatibility-mode blocked UDP to `LocalNetworkPermissionDenied` while documenting temporary `NEARBY_WIFI_DEVICES` behavior. For target SDK 37 or higher it SHALL declare and request `ACCESS_LOCAL_NETWORK` after an in-context rationale and before discovery, hosting, or direct join.

#### Scenario: Target SDK is 36
- **WHEN** ordinary local-network restrictions are not compatibility-enabled
- **THEN** multiplayer relies on implicit LAN access from `INTERNET`
- **AND** it does not request `ACCESS_LOCAL_NETWORK`

#### Scenario: Target SDK is 37 or higher and permission is denied
- **WHEN** the user denies or later revokes `ACCESS_LOCAL_NETWORK`
- **THEN** discovery, hosting, and direct join are disabled
- **AND** the UI reports `LocalNetworkPermissionDenied`

### Requirement: Android network-loss handling
The observer SHALL retain the selected network handle plus initial interface, IPv4 address/prefix, and routes from ordered link-property callbacks. It SHALL register/use `onBlockedStatusChanged()` only on API level 29 or newer; API levels 24–28 SHALL rely on permission checks, `onLost`, capability/link-property callbacks, and mapped socket errors. Loss, replacement, blocking where observable, loss of IPv4, or material interface/address/prefix/route change SHALL enter `Stopping(Failed(error))`, restore `LocalMP` only after cleanup, pause emulation, and eventually publish `NetworkLost`, `NetworkChanged`, or `LocalNetworkPermissionDenied`. Protocol v2 SHALL NOT silently migrate an active session.

#### Scenario: Device changes access points
- **WHEN** Android replaces the selected Wi-Fi network during a lobby
- **THEN** the current session fails with `NetworkChanged`
- **AND** emulation remains paused until the user acknowledges the modal failure

#### Scenario: DHCP changes IPv4 on the same network handle
- **WHEN** `onLinkPropertiesChanged` reports a material IPv4 or route change without `onLost`
- **THEN** the current session fails with `NetworkChanged`

### Requirement: Host and DS host independence
The UI and session model SHALL distinguish the application host from the emulated DS command host. The Phase 0 candidate star SHALL allow any connected endpoint, including a client, to originate a DS command exchange without taking ownership of the application session. Production behavior SHALL follow the topology recorded by the feasibility gate.

#### Scenario: Client becomes DS host
- **WHEN** a client endpoint sends a valid DS command
- **THEN** replies for that command are routed to that client
- **AND** application membership, admission, and lock authority remain with the application host

### Requirement: Qualified session capacity
Protocol v2 SHALL retain `protocolMaximumPlayers = 16`, IDs `0..15`, and wire fields capable of representing sixteen members. The initially enabled build SHALL use `qualifiedMaxPlayers = 4`. Host configuration, validation, ENet peer count, discovery advertisement, UI selection, release enablement, and public claims SHALL allow only `2..qualifiedMaxPlayers` members including the host. A valid hello received at configured capacity SHALL receive `SessionFull`. Raising the qualified value to 8 or 16 SHALL require a simultaneous physical-device run at that size under the selected topology, the same latency/deadline criteria, fan-out/queue evidence, and an updated decision record.

#### Scenario: Fifth participant targets the initial build
- **WHEN** a session configured for four members already has four accepted members
- **THEN** the next valid hello is rejected with `SessionFull`

#### Scenario: Wire codec represents sixteen members
- **WHEN** protocol-only tests encode IDs 0 through 15 and a sixteen-entry membership snapshot
- **THEN** the codec accepts the representable protocol maximum
- **AND** production host admission remains capped at `qualifiedMaxPlayers`

#### Scenario: Capacity qualification is raised
- **WHEN** a proposal raises `qualifiedMaxPlayers` to eight or sixteen
- **THEN** dated physical evidence at that simultaneous device count and an updated topology/capacity record are required before enablement

### Requirement: Persist only user-facing multiplayer preferences
The app SHALL persist player name, session name, maximum players, session port, discovery enabled, and diagnostics enabled. Queue limits, timeouts used by protocol correctness, sequence windows, expiry rules, and ENet flags SHALL remain validated implementation constants.

#### Scenario: User returns to multiplayer setup
- **WHEN** the user opens the overlay in a later emulator session
- **THEN** the six user-facing preferences are restored
- **AND** no prior live membership or connection identity is restored

### Requirement: Typed and actionable UI failures
The UI SHALL distinguish `LocalNetworkPermissionDenied`, `NetworkBindFailed`, `PortUnavailable`, `HostNameResolvedNoIPv4`, `LegacyOrUnknownProtocol`, `PeerHelloTimeout`, invalid state, local Wi-Fi, address, resolution, timeout, unreachable host, discovery, network loss/change, queue/router capacity, stopping, protocol, control overflow, shutdown timeout, and worker failure. A discovery-only failure SHALL NOT terminate a successfully hosted direct-connect session when local-network permission is granted.

#### Scenario: Host advertisement cannot be sent
- **WHEN** the ENet host is accepting direct connections but UDP discovery fails
- **THEN** the lobby remains usable
- **AND** it reports direct-connect-only status rather than a fatal session failure

#### Scenario: Active LAN fails fatally
- **WHEN** host loss, network change, protocol failure, or worker failure ends a live LAN session
- **THEN** the app restores `LocalMP` for memory safety but pauses emulation
- **AND** it requires explicit acknowledgement before isolated gameplay can continue

### Requirement: Android release qualification
Release enablement SHALL require content-free automated tests plus a documented physical-device matrix at `qualifiedMaxPlayers`. The initial matrix SHALL contain four simultaneous Android devices, multiple Android API levels or OEM Wi-Fi stacks, a host-not-equal-to-DS-host exchange, a known-supported multi-console workflow, a fixed-duration soak, application-host loss during exchange, DS-host departure while the application host remains, repeated fresh joins while unlocked, above-MTU traffic under loss, and physical ARM64 interoperability. An x86_64 Android emulator to ARM64 physical-device protocol run SHALL also be recorded when the emulator network environment permits local peer access.

#### Scenario: Feature is proposed for release
- **WHEN** the feature flag is to be enabled in a release build
- **THEN** all mandatory matrix cases have dated pass/fail evidence
- **AND** any retail-title qualification uses separately obtained user content outside automated fixtures
