## ADDED Requirements

### Requirement: Fixed protocol-v2 envelope
Every discovery datagram and ENet application packet SHALL begin with the normative 32-byte little-endian envelope defined in `design.md`: raw `MLMP` magic, framing version, protocol major/minor, message type, zero flags, header length, payload length, zero reserved field, session ID, sequence, and stream ID. The total received length SHALL equal `32 + payload_length`, the complete application packet SHALL NOT exceed 4096 bytes, and no native structure layout SHALL appear on the wire.

#### Scenario: Golden envelope is encoded
- **WHEN** the codec encodes a protocol-major-2 message with known field values
- **THEN** its bytes exactly match the checked-in golden little-endian vector
- **AND** decoding that vector reconstructs the same values on every supported ABI

#### Scenario: Packet has trailing bytes
- **WHEN** a packet contains bytes after its declared payload
- **THEN** the codec rejects it as malformed before session routing

#### Scenario: Declared payload exceeds received data
- **WHEN** the declared payload length is larger than the available bytes
- **THEN** the codec rejects it before allocation or copy

### Requirement: Stable bootstrap across protocol majors
Framing version 1, the common envelope, and the `JoinRejected` payload SHALL remain parseable independently of a future protocol-major payload schema that adopts framing v1. A host receiving a recognizable framing-v1 hello for an unsupported protocol major SHALL send `ProtocolMajorMismatch`, and a client SHALL present that rejection even though the server envelope carries a different protocol major. Legacy protocol v1 does not share this framing and SHALL produce `LegacyOrUnknownProtocol` when no framing-v1 exchange can be completed.

#### Scenario: Major versions differ
- **WHEN** a framing-v1 client hello advertises a protocol major other than 2
- **THEN** the host sends the stable rejection with its supported major/minor, core epoch, required features, and diagnostic build ID

#### Scenario: Legacy direct peer connects
- **WHEN** received bytes or bootstrap timeout do not identify framing version 1
- **THEN** the session reports `LegacyOrUnknownProtocol`
- **AND** it does not promise a parseable rejection to the legacy peer

### Requirement: Direct and discovered join session IDs
A discovered `ClientHello` SHALL carry the advertised nonzero session ID. A direct-join `ClientHello` SHALL carry the all-zero session-ID sentinel. `ClientHello`, `ServerWelcome`, and `JoinRejected` SHALL use sequence 1 and stream zero. Welcome and rejection SHALL carry the host's current authoritative nonzero session ID, and every accepted post-welcome message SHALL carry that exact ID and the sender's assigned stream. A client SHALL remain in `Joining` after welcome until it validates the first authoritative membership snapshot.

#### Scenario: Direct join succeeds
- **WHEN** a client connects by address without a prior advertisement and sends session ID zero
- **THEN** the host may accept it
- **AND** the client adopts the nonzero session ID from `ServerWelcome`

#### Scenario: Advertised session was replaced
- **WHEN** a discovered client sends a nonzero expected session ID that no longer names the host's current session
- **THEN** the host rejects it with `SessionReplaced`

### Requirement: Explicit compatibility negotiation
Both peers SHALL exchange protocol major/minor, `core_network_epoch`, supported features, required features, and diagnostic build ID during bootstrap. Protocol major 2 SHALL initially require feature bits 0 through 3 and core-network epoch 1. A peer SHALL reject an absent required feature or epoch mismatch, SHALL ignore unknown optional supported bits, and SHALL NOT reject solely because build identifiers differ.

#### Scenario: Build identifiers differ
- **WHEN** two peers have different build hashes but compatible protocol, epoch, and feature sets
- **THEN** the join remains eligible
- **AND** both build identifiers are retained only for diagnostics

#### Scenario: Required feature is absent
- **WHEN** either peer requires a bit the other does not support
- **THEN** the host rejects the join with `RequiredFeatureUnsupported`

#### Scenario: Core epochs differ
- **WHEN** the client and host advertise different core-network epochs
- **THEN** the host rejects the join with `CoreEpochMismatch`

### Requirement: Canonical text fields
Player names SHALL be valid shortest-form UTF-8 of 1 through 31 bytes; session names SHALL be valid shortest-form UTF-8 of 1 through 63 bytes; and diagnostic build IDs SHALL be 0 through 64 bytes from U+0020 through U+007E. Names SHALL reject U+0000, U+0001..U+001F, U+007F..U+009F, U+202A..U+202E, U+2066..U+2069, and invalid or overlong UTF-8. Length fields SHALL count encoded bytes.

#### Scenario: Multibyte name fits the byte limit
- **WHEN** a valid player name uses multiple UTF-8 bytes but totals no more than 31 bytes
- **THEN** the codec accepts it without truncation

#### Scenario: Name exceeds byte limit
- **WHEN** a player-name field declares or decodes to more than 31 bytes
- **THEN** the hello is rejected with `InvalidName`

### Requirement: Exact message schemas
The codec SHALL implement only the v2 message types and exact byte offsets defined in `design.md`: `ClientHello`, `ServerWelcome`, `JoinRejected`, `MembershipSnapshot`, `EndpointState`, `Leave`, `HostShutdown`, `DSFrame`, `ChannelBarrier`, and `DiscoveryAdvertisement`. `HostShutdown` SHALL be exactly four payload bytes: unsigned little-endian 16-bit reason at offset 0 and a zero 16-bit reserved field at offset 2. Every fixed reserved field SHALL be zero, every enum/boolean SHALL be in range, and trailing per-message fields SHALL be rejected.

#### Scenario: Reserved field is nonzero
- **WHEN** a received message has a nonzero field designated reserved by the schema
- **THEN** the receiver treats the message as a protocol violation

#### Scenario: Message arrives on the wrong channel
- **WHEN** a control message arrives on the DS-frame channel or a DS frame arrives on the control channel
- **THEN** the receiver rejects it before forwarding anything to the core

#### Scenario: Host shutdown reserved bytes are nonzero
- **WHEN** a four-byte `HostShutdown` has any nonzero byte at offsets 2 or 3
- **THEN** the codec rejects it as a protocol violation

#### Scenario: Host shutdown golden vector is encoded
- **WHEN** the codec encodes host-shutdown reason `3`
- **THEN** its payload bytes are exactly `03 00 00 00`

### Requirement: Discovery is bounded and source-authoritative
Discovery SHALL use IPv4 UDP broadcast port 7063, advertise at one-second intervals, and expire entries after five seconds without a newer advertisement. The session port SHALL default to 7064 and be configurable from 1024 through 65535 except 7063. The host endpoint address SHALL come from the UDP source address, and entries SHALL be deduplicated by `(session_id, source_ipv4, session_port)`. Advertised maximum players SHALL be within `2..qualifiedMaxPlayers`, even though the one-byte wire field can represent the protocol maximum of 16.

#### Scenario: Advertisement encodes no target IP
- **WHEN** a client receives a valid advertisement
- **THEN** it associates the advertised session metadata with the datagram source IPv4 address
- **AND** no payload value can redirect the join to another IP address

#### Scenario: Advertisement expires
- **WHEN** no newer advertisement for a discovered key arrives for 5000 milliseconds of monotonic time
- **THEN** the session is removed from the discovery snapshot

#### Scenario: Host cannot advertise
- **WHEN** discovery socket setup or broadcast fails after the ENet host starts
- **THEN** direct joining remains available
- **AND** discovery failure is reported separately

### Requirement: Authoritative full membership
The application host SHALL assign player identities and publish complete membership snapshots sorted by player ID. The host SHALL increment a nonzero 32-bit membership revision for every accepted membership, endpoint-active, or lock change. A receiver SHALL apply only a revision that is newer under the specified modular comparison, and an unconsumed snapshot MAY be coalesced only by replacing it with a newer complete snapshot. The first client snapshot SHALL contain ID 0 with the exact host generation/stream from `ServerWelcome`, the exact assigned client identity from that welcome, and a revision equal to or newer than the welcome revision.

#### Scenario: Player disconnects
- **WHEN** the application host confirms a peer disconnect
- **THEN** it removes that identity
- **AND** it publishes a newer full membership snapshot

#### Scenario: Older snapshot arrives late
- **WHEN** a client has applied revision 20 and later receives revision 19
- **THEN** it discards revision 19 without changing membership

#### Scenario: Welcome and initial host identity disagree
- **WHEN** the ID-0 entry in the first snapshot differs from the host generation or stream established by `ServerWelcome`
- **THEN** the client rejects the session with `ProtocolViolation`
- **AND** it does not enter `Lobby`

#### Scenario: Assigned client is absent from initial snapshot
- **WHEN** the first snapshot does not contain the exact client ID, generation, and stream assigned by `ServerWelcome`
- **THEN** the client rejects the session before installing a usable lobby

### Requirement: Player generation and stream identity
At session creation the application host SHALL create its local identity as player ID 0, generation 1, a cryptographically random nonzero 32-bit stream ID unique among live identities, and no ENet transport peer. `ServerWelcome` SHALL explicitly carry host ID 0, host generation, and host stream in addition to the assigned client identity. Clients SHALL use IDs 1 through 15 subject to the configured maximum not exceeding `qualifiedMaxPlayers`. A client slot's first assignment SHALL use generation 1; every reuse SHALL increment its 16-bit generation and SHALL never use zero. A slot that would wrap generation SHALL be retired for the session. Every accepted client connection SHALL receive a random nonzero 32-bit stream ID unique among live identities. Routing and sequence windows SHALL use `(session_id, player_id, player_generation, stream_id)`.

#### Scenario: Delayed frame follows slot reuse
- **WHEN** a frame from a prior generation arrives after that player ID has been reassigned
- **THEN** the router drops it before core delivery

#### Scenario: Stream identity is replayed
- **WHEN** a peer sends a message using its old stream ID after a fresh join
- **THEN** the host rejects or drops the message as stale

#### Scenario: Host session is created
- **WHEN** the application host creates a new session before accepting peers
- **THEN** member 0 has generation 1, a random nonzero stream, and no ENet peer
- **AND** the first membership revision contains that same identity

### Requirement: Application sequence behavior
Every assigned stream SHALL maintain six independent sequence generators: control, regular, command, reply, ack, and channel barrier. Each SHALL start at 1, increment modulo `2^32`, and skip zero. `candidate` SHALL be newer than `last` exactly when `int32(candidate - last) > 0`. Receivers SHALL accept the first nonzero sequence and thereafter only newer sequences in a window keyed by session, player, generation, stream, and the same class. Relays SHALL preserve the logical origin's class sequence; `reply_to_command_sequence` SHALL name the command-class sequence.

#### Scenario: Sequence wraps
- **WHEN** an accepted sequence is `0xFFFFFFFF` and the next valid sender sequence is 1
- **THEN** the receiver accepts 1 as newer

#### Scenario: Duplicate frame arrives
- **WHEN** a frame repeats an already accepted identity and sequence
- **THEN** the router discards it deterministically

### Requirement: Exact core frame limits
Regular, command, and ack DS frames SHALL contain 36 through `0x948` bytes. Payload replies SHALL contain 36 through 1024 bytes. Their internal little-endian frame length at bytes 10..11 SHALL equal `total_length - 12`. Only an AID-0 default reply SHALL be zero length. Outbound undersize, oversize, or internally inconsistent frames SHALL be rejected before allocation/queueing, and inbound equivalents SHALL be rejected before copy.

#### Scenario: Maximum ordinary frame is sent
- **WHEN** the core submits exactly `0x948` ordinary bytes
- **THEN** the frame is eligible for encoding and unreliable fragmentation

#### Scenario: Reply exceeds its core slot
- **WHEN** a reply contains 1025 bytes
- **THEN** it is rejected before queueing

#### Scenario: Default zero-length reply is sent
- **WHEN** the core submits a zero-length AID-0 default reply with a live inbound command context
- **THEN** the transport records that responder for the correlated exchange
- **AND** no ordinary zero-length frame is delivered to `Wifi::CheckRX()`

### Requirement: DS frame identity validation
Every DS frame SHALL carry the logical DS origin, class, AID, destination identity, emulated timestamp, command correlation, declared length, and bytes defined in `design.md`. On the host, a client-originated logical identity SHALL exactly match that transport peer. On a client, the transport peer SHALL be the application host while logical origin MAY be any host-authorized current identity. Host-local delivery and relays SHALL preserve logical origin, bytes, timestamp, class sequence, and command correlation. Control messages SHALL remain transport-peer-bound.

#### Scenario: Client forges another sender
- **WHEN** a client sends a DS payload whose player ID, generation, or stream differs from its host-assigned identity
- **THEN** the host rejects it before relay

#### Scenario: Client receives relayed command
- **WHEN** client B receives client A's command from its application-host ENet peer
- **THEN** B validates the transport peer as application host
- **AND** accepts A as the preserved logical DS origin

#### Scenario: Client receives relayed regular frame
- **WHEN** client B receives client A's regular frame through the application host
- **THEN** B preserves A as logical origin during core delivery
- **AND** does not rewrite the frame as application-host-originated

#### Scenario: Relayed frame overtakes membership snapshot
- **WHEN** a host-authorized frame for a new member reaches a client before the channel-0 membership snapshot
- **THEN** the client accepts the logical origin under application-host authority
- **AND** UI membership catches up independently without delaying core delivery

### Requirement: Exchange-correlated reply routing
Each accepted command SHALL create a bounded exchange keyed by its DS-host identity and command-class sequence. Expected responders SHALL be accepted, endpoint-active members at command acceptance excluding the logical origin. The record SHALL snapshot those identities, command emulated timestamp, host monotonic acceptance time, and monotonic expiry `min(500 ms, max(100 ms, 4 × core_receive_timeout))`. Replies SHALL identify that command and its exact DS-host destination and arrive before expiry. The router SHALL retain timestamps for diagnostics but SHALL NOT compare cross-device emulated clocks. The exchange SHALL end on matching ack, expiry, or DS-host departure/generation change.

#### Scenario: Delayed reply follows a newer command host
- **WHEN** player A's reply arrives after player B has sent another command
- **THEN** the reply is routed only if it still matches player A's unexpired exchange
- **AND** it is never redirected to player B merely because B was the latest command sender

#### Scenario: DS host departs
- **WHEN** the identity that originated a command disconnects before replies arrive
- **THEN** its exchange is removed
- **AND** later replies for it are discarded

#### Scenario: Expected responder becomes inactive
- **WHEN** an expected responder leaves or becomes endpoint-inactive during an exchange
- **THEN** the host removes it from the expected set
- **AND** an endpoint becoming active later is not added to that exchange

#### Scenario: Relay enqueue fails locally
- **WHEN** the host cannot admit one destination's command relay into its bounded outgoing path
- **THEN** that destination is removed from the expected responder set
- **AND** a per-destination `RelayQueueDrop` diagnostic distinguishes local overload from later network loss

### Requirement: Association-ID reply rules
Payload-bearing replies SHALL use AID 1 through 15. AID 0 SHALL be legal only for a zero-length default reply. Only the first reply from a responder for an exchange SHALL be accepted; for payload replies, only the first accepted value for an AID SHALL occupy the core's 1 KiB slot.

#### Scenario: Zero-length default reply arrives
- **WHEN** an expected responder sends a zero-length reply with AID 0 for a live exchange
- **THEN** it is accepted as that responder's default reply
- **AND** it does not occupy a payload AID slot

#### Scenario: Payload uses AID zero
- **WHEN** a reply with AID 0 contains one or more bytes
- **THEN** the router rejects it as malformed

#### Scenario: Two responders claim one AID
- **WHEN** two valid payload replies target the same exchange and AID
- **THEN** the first accepted payload remains in that core slot
- **AND** the later collision is dropped deterministically

### Requirement: Explicit core-adapter command contexts
The adapter SHALL retain one inbound command context, one outbound command context, and one logical last-host context per local instance. Before returning a command from `RecvPacket()` or `RecvHostPacket()`, it SHALL set inbound logical sender/generation/stream, command sequence, and timestamp; a subsequent accepted `SendReply()` SHALL reference and consume it. A second command SHALL NOT be delivered until the context is consumed or expires. An accepted `SendCmd()` SHALL create the sole outbound context; another accepted command SHALL deterministically supersede the prior context and discard its pending replies. `SendAck()` SHALL reference and close the current outbound context.

#### Scenario: Reply follows a delivered command
- **WHEN** `RecvHostPacket()` returns client A's relayed command and the local core calls `SendReply()`
- **THEN** the adapter targets A's exact identity and command sequence from retained inbound context

#### Scenario: Another command arrives before reply
- **WHEN** a second command reaches the adapter while inbound context remains live
- **THEN** the second command remains in the bounded queue and is not exposed to the core
- **AND** it can be delivered only after consumption or expiry of the first context

#### Scenario: Local instance sends overlapping commands
- **WHEN** `SendCmd()` is accepted while an outbound command context is still pending
- **THEN** the prior context is marked superseded and its pending replies are discarded
- **AND** subsequent `RecvReplies()` and `SendAck()` reference only the new command

### Requirement: Core-adapter wait and return behavior
`RecvReplies()` SHALL zero all fifteen 1-KiB AID slots before aggregating only the current outbound exchange. When the application host is also the logical DS host, it MAY return on requested-mask completion, authoritative exhaustion of expected responders, facade receive timeout, or stop/failure. When a client is the logical DS host, it SHALL return only when `(receivedAidMask & requestedAidMask) == requestedAidMask`, the facade receive timeout expires, or stop/failure occurs; it SHALL NOT infer exhaustion from local membership. Cross-device timestamp stale filtering SHALL be replaced by explicit exchange identity. On stop/network failure, waits SHALL wake immediately: `RecvPacket()` returns 0, `RecvHostPacket()` returns -1, and `RecvReplies()` returns 0 after zero-fill. Nonzero send calls SHALL return submitted length only after queue admission and 0 on rejection. A zero-length AID-0 reply SHALL retain the pinned API's ambiguous return value 0 while internal diagnostics record admission/drop.

#### Scenario: Logical DS host leaves but application host remains
- **WHEN** the retained logical last-host identity leaves, becomes endpoint-inactive, or changes generation/stream
- **THEN** `RecvHostPacket()` returns -1
- **AND** the surviving application-host connection does not mask DS-host departure

#### Scenario: Session stops during reply wait
- **WHEN** `RecvReplies()` is blocked and stop or network failure occurs
- **THEN** all reply slots are zero
- **AND** the call wakes and returns 0 without waiting for the original deadline

#### Scenario: Client DS host has stale membership
- **WHEN** a client-originated command includes an AID not yet present in that client's local membership snapshot
- **THEN** its adapter does not terminate because locally known responders appear exhausted
- **AND** it waits for requested-mask completion, timeout, or session stop/failure

#### Scenario: Application host is the DS host
- **WHEN** the application host's local adapter waits on an exchange whose authoritative expected responders have all replied, departed, or become ineligible
- **THEN** it may return before timeout even when the requested mask is not otherwise complete

### Requirement: Application-host star routing
The Phase 0 candidate star SHALL give each client one ENet connection to the application host. An application host SHALL allocate exactly `qualifiedMaxPlayers - 1` ENet peer slots; a client SHALL allocate exactly one. The host SHALL validate and relay accepted frames to eligible endpoint-active clients and its local core adapter. Clients SHALL NOT connect directly to one another or interpret membership-changing requests as authoritative. Production star routing SHALL NOT be approved unless the physical timing gate passes; a failed gate requires this requirement to be amended to the selected fallback topology.

#### Scenario: Four-device session forms
- **WHEN** three clients join one application host
- **THEN** each client has one ENet peer connection
- **AND** all cross-client DS traffic transits the application host

#### Scenario: Relayed round trip preserves identities
- **WHEN** A sends a command through the host to B, B replies through the host to A, and A sends an ack through the host to B
- **THEN** every relay preserves A or B as the appropriate logical origin
- **AND** each receiver validates the application host only as its transport peer

### Requirement: Exact ENet delivery policy
ENet SHALL use two channels. Channel 0 control and membership SHALL use `ENET_PACKET_FLAG_RELIABLE`. Channel 1 DS frames SHALL use only `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT`, never `RELIABLE` or `UNSEQUENCED`. Before a peer's channel-1 unreliable counter reaches 60,000, the worker SHALL send a reliable zero-payload `ChannelBarrier` on channel 1 and consume it in the session layer so ENet resets the unreliable sequence space before exhaustion. The total join deadline SHALL be 5000 monotonic milliseconds; connected peers SHALL use a 500-ms ping interval and ENet timeout parameters `(4, 1000 ms, 5000 ms)`.

#### Scenario: DS frame exceeds path MTU
- **WHEN** a DS frame requires ENet fragmentation
- **THEN** all fragments use ENet's unreliable-fragment command
- **AND** loss of a fragment drops the frame rather than retransmitting it

#### Scenario: Data channel approaches rollover
- **WHEN** a peer approaches 60,000 channel-1 unreliable sends
- **THEN** a reliable barrier resets ENet's unreliable sequence space before another DS frame is queued
- **AND** the barrier is not forwarded to the emulated core

#### Scenario: Rollover barrier is lost
- **WHEN** UDP loss delays the reliable channel barrier
- **THEN** ENet may temporarily stall later channel-1 unreliable delivery while retransmitting the barrier
- **AND** no DS payload itself is promoted or retransmitted

#### Scenario: Control is lost at UDP level
- **WHEN** UDP loss affects a channel-0 control packet
- **THEN** ENet retransmits and preserves order for that control message

### Requirement: Copied bounded queues
Transport boundaries SHALL contain copied value objects rather than `ENetPacket*`. The implementation SHALL enforce the entry, byte, and age limits in `design.md` for local commands, per-peer control, membership snapshots, both DS directions, and diagnostics.

#### Scenario: Remote control budget is exhausted
- **WHEN** one peer exceeds its bounded inbound control budget
- **THEN** no accepted critical control message is silently evicted
- **AND** the host disconnects that peer with `ControlOverflow`

#### Scenario: Membership updates outpace UI
- **WHEN** several full membership snapshots await consumption
- **THEN** only the newest valid revision needs to remain queued

#### Scenario: DS queue is saturated
- **WHEN** a command frame arrives to a full DS queue that contains regular frames
- **THEN** the oldest regular frame may be evicted to admit the command
- **AND** no session-control item is affected

### Requirement: Bounded ENet and auxiliary memory
Immediately after every `enet_host_create(nullptr, ...)` and before admission, `maximumPacketSize` SHALL be 4096 and `maximumWaitingData` SHALL be 65536 per peer. Reliable outgoing data SHALL remain charged until ENet's packet-release callback, with limits of 65536 bytes per peer and 524288 bytes per host. Pending unwelcomed peers SHALL be bounded to `qualifiedMaxPlayers - 1` and peer diagnostic records to `qualifiedMaxPlayers`, with protocol hard ceilings of 15 and 16 respectively. The implementation SHALL enforce the design's numeric bounds for exchanges, discovery entries, rate-limit sources, resolver requests, sequence windows, and peer diagnostics.

#### Scenario: Oversize fragments arrive before codec
- **WHEN** a peer declares/reassembles an application packet larger than 4096 bytes
- **THEN** ENet rejects it under `maximumPacketSize`
- **AND** the codec is not relied upon as the first memory bound

#### Scenario: Reliable peer is wedged
- **WHEN** unacknowledged reliable control reaches that peer's retained-outgoing budget
- **THEN** no additional reliable packet is admitted for that peer
- **AND** its charge remains until ENet releases the packet

#### Scenario: Auxiliary table reaches capacity
- **WHEN** a bounded auxiliary collection is full after expiry/purge
- **THEN** its specified deterministic reject or eviction rule is applied
- **AND** memory does not grow with session duration

### Requirement: Selected-network ENet construction
Every ENet host SHALL be created with a null address and role-specific peer count, capped, bound to the selected Android network with `android_setsocknetwork(host->socket)`, and only then manually bound for hosting or connected for joining. The application host SHALL bind `ENET_HOST_ANY` with the selected port after network attachment; a wildcard result from `enet_socket_get_address()` is valid, and the displayed direct-join IPv4 address SHALL instead come from the retained selected-network `LinkProperties`. Discovery clients SHALL continue to use the received datagram source IPv4. Host bind failure SHALL map to `NetworkBindFailed` or `PortUnavailable`. Android hostname resolution SHALL use blocking selected-network DNS and pass only IPv4 bytes to native.

#### Scenario: Application host starts
- **WHEN** hosting begins on a selected Wi-Fi network
- **THEN** ENet creates an unbound socket with `qualifiedMaxPlayers - 1` peers, applies hard caps, binds it to the Android network, manually binds wildcard plus selected port, and verifies the port in that order
- **AND** it displays the selected `LinkProperties` IPv4 for direct join while discovery recipients use the datagram source address

#### Scenario: Client starts
- **WHEN** joining begins
- **THEN** ENet creates exactly one peer slot and binds its socket to the selected network before connect

#### Scenario: Port is already occupied
- **WHEN** manual ENet socket bind reports address-in-use
- **THEN** startup fails with `PortUnavailable`
- **AND** no partially initialized worker remains

#### Scenario: VPN is process default
- **WHEN** selected-network DNS and ENet run while a VPN is the process default route
- **THEN** name resolution and UDP traffic use the selected Wi-Fi network without process-wide binding

### Requirement: Guaranteed stopping path
`Stop` SHALL be idempotent and use an out-of-band atomic flag plus descriptor/condition wake. `Leave` and `Cancel` SHALL each have a named reserved command slot. All worker sockets SHALL be nonblocking, `enet_host_service()` SHALL wait no more than 5 ms, and synchronous DNS SHALL NOT run on the worker. Only the worker SHALL reset/destroy ENet and signal cleanup completion; the controller SHALL join only after that signal. Two seconds SHALL be the measured teardown SLO, never a cross-thread destruction deadline. Any operation with live cleanup SHALL remain `Stopping(terminalTarget)` and SHALL NOT become retryable `Failed` before cleanup, join, and facade restoration complete.

#### Scenario: Stop is requested under full load
- **WHEN** all ordinary command capacity is occupied
- **THEN** stop still wakes the worker
- **AND** the worker releases ENet and socket ownership within the bounded shutdown policy

#### Scenario: Teardown exceeds the SLO
- **WHEN** worker cleanup has not completed after 2000 ms
- **THEN** the facade remains gated, emulation/runtime cleanup remains paused/deferred, and the state remains `Stopping`
- **AND** the controller reports visible `ShutdownTimeout` while continuing to await worker-owned completion
- **AND** it enters `Failed(ShutdownTimeout)` only after cleanup and join

### Requirement: Strict malformed-input handling
Before allocation, copy, membership mutation, sequence-window mutation, exchange creation, relay, or core delivery, the receiver SHALL validate envelope, total length, message schema, UTF-8, enum ranges, reserved bits, session, peer identity, generation, stream, sequence, frame class, AID, correlation, and class-specific length. Three nonfatal malformed application messages in a rolling ten-second interval SHALL disconnect that peer. Oversize application packets, a wrong post-welcome session, forged client identity, and control overflow SHALL disconnect immediately. Malformed discovery datagrams SHALL be dropped without affecting a live session.

#### Scenario: Fuzzer supplies arbitrary bytes
- **WHEN** arbitrary or truncated input is decoded under ASan and UBSan
- **THEN** the codec returns a bounded error
- **AND** no out-of-bounds access, use-after-free, leak, undefined behavior, or unbounded allocation occurs

#### Scenario: Malformed discovery traffic is received
- **WHEN** a datagram on port 7063 fails envelope or advertisement validation
- **THEN** it is discarded
- **AND** existing discovery entries and LAN peers remain unchanged

### Requirement: Bounded join attempts
The host SHALL accept at most eight `ClientHello` attempts from one source IPv4 address in a rolling ten-second interval. Further attempts SHALL receive `RateLimited` when possible and SHALL be ignored for 30 seconds. A connected peer SHALL send a valid hello within 1500 monotonic milliseconds or be disconnected with `PeerHelloTimeout`.

#### Scenario: One source floods hello
- **WHEN** one IPv4 source exceeds eight hello attempts inside ten seconds
- **THEN** the host does not allocate another membership identity for that source during the 30-second limit
- **AND** other source addresses remain eligible

#### Scenario: Peer connects without hello
- **WHEN** 1500 ms elapse after ENet connect without a valid `ClientHello`
- **THEN** the host disconnects that peer
- **AND** frees its pending peer slot with `PeerHelloTimeout`

### Requirement: One worker owns transport state
Only the ENet worker SHALL create, service, mutate, or destroy ENet hosts, peers, packets, discovery sockets, accepted resolver results, and native network bindings. Blocking network-scoped DNS SHALL run separately through at most four executing/queued generation-tagged operations. Cancellation SHALL be logical: capacity remains charged until Android returns, while stale-generation results are ignored. Core callbacks, JNI callers, resolver completion, and UI threads SHALL communicate with the worker only through bounded commands/events and immutable snapshots.

#### Scenario: UI leaves while core sends
- **WHEN** the UI requests leave concurrently with a core send callback
- **THEN** neither thread directly destroys ENet state
- **AND** the stable facade and worker serialize shutdown without use-after-free

### Requirement: Deterministic transport testing
Monotonic time, resolution, socket/network binding, datagram ingress/egress, and fault decisions SHALL be injectable. Unit tests SHALL deterministically cover loss, delay, jitter, duplication, reorder, queue age, join timeout, discovery expiry, exchange expiry, sequence wrap, channel barriers, and shutdown deadlines. Real ENet loopback tests SHALL separately cover integration.

#### Scenario: Reordering policy is tested
- **WHEN** the deterministic shim delivers sequences 10, 12, 11, and 12
- **THEN** the test reproducibly proves the specified accept/drop decisions without wall-clock sleeps

#### Scenario: Above-MTU loss is tested
- **WHEN** the shim drops one fragment of a maximum-size DS frame
- **THEN** the integration evidence shows no reconstructed core frame and no reliable retransmission
