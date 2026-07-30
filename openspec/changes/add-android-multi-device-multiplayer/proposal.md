## Why

`melonDS-android` currently lists local multiplayer as a missing feature even though its native core already exposes the multiplayer interface and the app already links ENet. Android users need a reliable way to host or join a local-network session across multiple physical devices without inheriting the desktop implementation's blocking joins, peer mesh, ABI-dependent protocol, unbounded queues, or unsafe backend lifetime.

## What Changes

- Fork `rafaelvcaetano/melonDS-android` at `ae8790bdb4bf01555c2f168ab9bc77fbeb5dceb6` and its `melonDS-android-lib` submodule at `431ab4bd0003c4356e25fce89640ae1004579b9b`; retain the existing ENet 1.3.18 submodule.
- Add host, discovery, direct-join, lobby, explicit session lock, membership, diagnostics, leave, and failure recovery flows to the Android emulator experience.
- Add an Android-aware session owner that survives activity recreation while the emulator session lives, acquires only the required Wi-Fi resources, applies a target-SDK-versioned local-network permission policy, owns the API-29+ low-latency `WifiLock` experiment under the normal `WAKE_LOCK` permission, and reports local-network changes as typed failures.
- Replace native multiplayer backend replacement with a stable, synchronized facade that tracks active emulated endpoints and safely replays their state when switching between `LocalMP` and LAN.
- Evaluate an application-host star as the candidate data topology using a minimal real-core vertical slice on two, three, and four physical Android devices; require a recorded latency/topology/capacity decision before full implementation.
- If the star timing gate passes, add the star topology with one ENet-owning worker, bounded copied-value queues and ENet memory, authoritative membership, player generations, per-connection stream identities, and exchange-correlated DS reply routing. If it fails, amend this package to require application host = DS host, direct DS-host data, or a hybrid topology before proceeding.
- Add protocol v2 with a normative byte-level codec, stable direct-join bootstrap, explicit application-host identity, feature negotiation, fixed limits, reliable ordered control, and sequenced unreliable-fragmented DS frames.
- Keep a wire-level `protocolMaximumPlayers` of 16 while initially setting `qualifiedMaxPlayers` to 4. Host configuration, admission, advertisements, UI, and release claims cannot exceed the highest physically qualified capacity.
- Add deterministic native transport/router tests, Kotlin state and lifecycle tests, Android instrumentation tests, and multi-device qualification.
- **BREAKING**: LAN protocol v2 intentionally does not interoperate with the desktop protocol-v1 native-struct format or the abandoned desktop-target proposal. A clean mismatch response is guaranteed only for protocols sharing the new framing-v1 bootstrap; legacy peers may only produce `LegacyOrUnknownProtocol`.
- Keep internet matchmaking, WAN traversal, encryption/authentication, host migration, reconnection reservation, rollback, emulator-core wireless accuracy work, and changes to `LocalMP` out of scope.

## Capabilities

### New Capabilities

- `android-multi-device-session`: Physical feasibility gate, Android host/join experience, versioned permissions and Wi-Fi lifecycle, session state, membership, locking, backend switching, and failure behavior.
- `multiplayer-frame-transport`: Protocol-v2 framing, compatibility negotiation, discovery, transport-peer versus logical-origin identity, core correlation, routing, queue/ENet memory limits, delivery policy, and malformed-input handling.

### Modified Capabilities

None.

## Impact

- Parent repository: Android UI, ViewModel/repository state, settings, permissions, Wi-Fi network/resource handling, JNI declarations, Gradle/manifest wiring, and physical-device tests.
- Native-core submodule: `MPInterface`, LAN backend, protocol codec, router, ENet worker, core adapter, copied queues, and native tests.
- Fork maintenance: the parent fork pins the project-owned native-core fork; both retain upstream remotes and document their exact baselines.
- Approval boundary: only fork setup, design-correction support, selected-network transport, the minimal real-core vertical slice, teardown proof, instrumentation, and the physical timing gate are initially authorized. Full lobby/UI/diagnostics work remains blocked until a decision record contains `selectedTopology`, `qualifiedMaxPlayers`, `testedDeviceMatrix`, `latencyGateResult`, and known device/network restrictions.
- Capacity: the initial enabled implementation is limited to 2–4 players. Raising `qualifiedMaxPlayers` to 8 or 16 requires a corresponding simultaneous physical-device capacity qualification and an updated decision record; representable protocol capacity alone is not a release claim.
- Compatibility: peers must share protocol major 2, the required feature set, and the maintained core-network epoch. Build hashes remain diagnostic and do not by themselves reject a join.
