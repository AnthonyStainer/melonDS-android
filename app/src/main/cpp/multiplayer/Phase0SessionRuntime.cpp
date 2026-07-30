#include "Phase0SessionRuntime.h"

#include <MPInterface.h>

#include <utility>

namespace MelonDSAndroid::Multiplayer
{
namespace
{

constexpr std::uint8_t ControlChannel = 0;
constexpr std::uint8_t DsChannel = 1;

}

std::unique_ptr<Phase0SessionRuntime> Phase0SessionRuntime::Create(
    RuntimeSettings settings)
{
    auto runtime = std::unique_ptr<Phase0SessionRuntime>(
        new Phase0SessionRuntime(std::move(settings)));
    return runtime->Initialize() ? std::move(runtime) : nullptr;
}

Phase0SessionRuntime::Phase0SessionRuntime(RuntimeSettings settings)
    : Settings(std::move(settings))
{
}

Phase0SessionRuntime::~Phase0SessionRuntime()
{
    Stop(std::chrono::milliseconds::max());
}

bool Phase0SessionRuntime::Initialize()
{
    Worker = std::make_unique<Phase0EnetWorker>(
        Settings.transport,
        [this](WorkerEvent event) {
            HandleWorkerEvent(std::move(event));
        });

    if (Settings.transport.role == WorkerRole::Host)
    {
        HostRouter = std::make_unique<Phase0SessionRouter>(
            Settings.playerName,
            Settings.sessionName,
            Settings.buildId,
            Settings.transport.qualifiedMaxPlayers,
            25,
            [this](const MemberIdentity& destination, const CoreFrameValue& frame) {
                return SendHostFrame(destination, frame);
            },
            [this](CoreFrameValue frame) {
                return Adapter != nullptr && Adapter->Deliver(std::move(frame));
            });
        SessionId = HostRouter->SessionId();
        ApplicationHostIdentity = HostRouter->HostIdentity();
        InstallAdapter(*ApplicationHostIdentity, true);
        CurrentState.store(RuntimeState::Starting);
    }
    else
    {
        CurrentState.store(RuntimeState::Joining);
    }
    return true;
}

bool Phase0SessionRuntime::Start()
{
    return Worker && Worker->Start();
}

bool Phase0SessionRuntime::Stop(std::chrono::milliseconds timeout)
{
    const RuntimeState prior = CurrentState.exchange(RuntimeState::Stopping);
    if (prior == RuntimeState::Stopped)
        return true;

    if (Adapter != nullptr)
        Adapter->StopSession();
    melonDS::MPInterface::Set(melonDS::MPInterface_Local);
    Adapter = nullptr;

    if (!Worker)
    {
        CurrentState.store(RuntimeState::Stopped);
        return true;
    }
    Worker->Stop();
    if (!Worker->WaitForStopped(timeout))
        return false;
    Worker->Join();
    CurrentState.store(
        CurrentError.load() == WorkerError::None
            ? RuntimeState::Stopped
            : RuntimeState::Failed);
    return true;
}

bool Phase0SessionRuntime::AwaitStopped(std::chrono::milliseconds timeout)
{
    if (!Worker || !Worker->WaitForStopped(timeout))
        return false;
    Worker->Join();
    CurrentState.store(
        CurrentError.load() == WorkerError::None
            ? RuntimeState::Stopped
            : RuntimeState::Failed);
    return true;
}

void Phase0SessionRuntime::InstallAdapter(
    MemberIdentity identity,
    bool applicationHostLocal)
{
    auto adapter = std::make_unique<Phase0LanCoreAdapter>(
        identity,
        applicationHostLocal,
        applicationHostLocal
            ? Phase0LanCoreAdapter::FrameSink(
                [this](CoreFrameValue frame) {
                    return HostRouter &&
                        HostRouter->SubmitHostLocal(std::move(frame));
                })
            : Phase0LanCoreAdapter::FrameSink(
                [this](CoreFrameValue frame) {
                    return SendClientFrame(std::move(frame));
                }),
        applicationHostLocal
            ? Phase0LanCoreAdapter::EndpointSink(
                [this, identity](bool active) {
                    if (HostRouter)
                        HostRouter->SetEndpointActive(identity, active);
                })
            : Phase0LanCoreAdapter::EndpointSink(
                [this](bool active) {
                    SendClientEndpoint(active);
                }),
        applicationHostLocal
            ? Phase0LanCoreAdapter::AuthoritativeExhaustion(
                [this](const MemberIdentity& origin, std::uint32_t sequence) {
                    return HostRouter &&
                        HostRouter->IsAuthoritativelyExhausted(origin, sequence);
                })
            : Phase0LanCoreAdapter::AuthoritativeExhaustion{});
    Adapter = adapter.get();
    melonDS::MPInterface::Install(
        std::move(adapter),
        melonDS::MPInterface_Phase0LAN);
}

void Phase0SessionRuntime::HandleWorkerEvent(WorkerEvent event)
{
    switch (event.type)
    {
    case WorkerEventType::Connected:
        if (Settings.transport.role == WorkerRole::Client)
        {
            Envelope envelope;
            envelope.messageType = MessageType::ClientHello;
            envelope.sequence = 1;
            ClientHello hello;
            hello.playerName = Settings.playerName;
            hello.buildId = Settings.buildId;
            if (!SendEncoded(
                    event.peerIndex,
                    ControlChannel,
                    ENET_PACKET_FLAG_RELIABLE,
                    envelope,
                    hello))
                Fail(WorkerError::QueueFull);
        }
        break;
    case WorkerEventType::Packet:
        if (!event.packet)
            break;
        if (Settings.transport.role == WorkerRole::Host)
            HandleHostPacket(event.peerIndex, *event.packet);
        else
            HandleClientPacket(*event.packet);
        break;
    case WorkerEventType::Disconnected:
        if (Settings.transport.role == WorkerRole::Host)
        {
            std::optional<MemberIdentity> departed;
            {
                std::lock_guard lock(Mutex);
                for (const auto& [identity, peer] : PeerByIdentity)
                {
                    if (peer == event.peerIndex)
                    {
                        departed = identity;
                        break;
                    }
                }
                if (departed)
                    PeerByIdentity.erase(*departed);
            }
            if (departed)
            {
                HostRouter->RemoveMember(*departed);
                if (Adapter)
                    Adapter->MemberUnavailable(*departed);
            }
        }
        else
        {
            Fail(WorkerError::WorkerFailed);
        }
        break;
    case WorkerEventType::Failure:
        if (event.error != WorkerError::PeerHelloTimeout)
            Fail(event.error);
        break;
    case WorkerEventType::Started:
    case WorkerEventType::Stopped:
        break;
    }
}

void Phase0SessionRuntime::HandleHostPacket(
    std::size_t peerIndex,
    const DecodedPacket& packet)
{
    if (const auto* hello = std::get_if<ClientHello>(&packet.payload))
    {
        HandleClientHello(peerIndex, packet.envelope, *hello);
        return;
    }
    if (const auto* endpoint = std::get_if<EndpointState>(&packet.payload))
    {
        MemberIdentity identity{
            endpoint->playerId,
            endpoint->generation,
            endpoint->streamId,
        };
        {
            std::lock_guard lock(Mutex);
            const auto peer = PeerByIdentity.find(identity);
            if (peer == PeerByIdentity.end() || peer->second != peerIndex)
            {
                Fail(WorkerError::ProtocolViolation);
                return;
            }
        }
        HostRouter->SetEndpointActive(identity, endpoint->active);
        return;
    }
    if (const auto* wireFrame = std::get_if<DSFrame>(&packet.payload))
    {
        if (!HostRouter->ReceiveFromClient(
                peerIndex,
                FromWireFrame(packet.envelope, *wireFrame)))
            Fail(WorkerError::ProtocolViolation);
        return;
    }
    if (!std::holds_alternative<ChannelBarrier>(packet.payload))
        Fail(WorkerError::ProtocolViolation);
}

void Phase0SessionRuntime::HandleClientPacket(const DecodedPacket& packet)
{
    if (const auto* welcome = std::get_if<ServerWelcome>(&packet.payload))
    {
        if (!ClientBootstrap.AcceptWelcome(packet.envelope.sessionId, *welcome))
        {
            Fail(WorkerError::ProtocolViolation);
            return;
        }
        SessionId = packet.envelope.sessionId;
        PendingWelcome = *welcome;
        ClientIdentity = MemberIdentity{
            welcome->playerId,
            welcome->playerGeneration,
            welcome->playerStreamId,
        };
        ApplicationHostIdentity = MemberIdentity{
            0,
            welcome->hostGeneration,
            welcome->hostStreamId,
        };
        return;
    }
    if (const auto* membership = std::get_if<MembershipSnapshot>(&packet.payload))
    {
        if (!ClientBootstrap.AcceptInitialMembership(*membership) ||
            !ClientIdentity)
        {
            Fail(WorkerError::ProtocolViolation);
            return;
        }
        if (Adapter == nullptr)
            InstallAdapter(*ClientIdentity, false);
        CurrentState.store(RuntimeState::Lobby);
        return;
    }
    if (const auto* frame = std::get_if<DSFrame>(&packet.payload))
    {
        CoreFrameValue value = FromWireFrame(packet.envelope, *frame);
        if (!ClientBootstrap.ValidateHostRelayedFrame(value) ||
            Adapter == nullptr ||
            !Adapter->Deliver(std::move(value)))
            Fail(WorkerError::ProtocolViolation);
        return;
    }
    if (const auto* rejection = std::get_if<JoinRejected>(&packet.payload))
    {
        static_cast<void>(rejection);
        Fail(WorkerError::ProtocolViolation);
        return;
    }
    if (!std::holds_alternative<ChannelBarrier>(packet.payload))
        Fail(WorkerError::ProtocolViolation);
}

void Phase0SessionRuntime::HandleClientHello(
    std::size_t peerIndex,
    const Envelope& envelope,
    const ClientHello& hello)
{
    JoinRejected rejection;
    if (envelope.sessionId != 0 && envelope.sessionId != SessionId)
        rejection.reason = 6;
    else if (hello.coreNetworkEpoch != CoreNetworkEpoch)
        rejection.reason = 5;
    else if ((hello.requiredFeatures & BaselineFeatures) != hello.requiredFeatures ||
        (hello.supportedFeatures & BaselineFeatures) != BaselineFeatures)
        rejection.reason = 4;

    if (rejection.reason != 0)
    {
        rejection.serverBuildId = Settings.buildId;
        Envelope rejectedEnvelope;
        rejectedEnvelope.messageType = MessageType::JoinRejected;
        rejectedEnvelope.sessionId = SessionId;
        rejectedEnvelope.sequence = 1;
        SendEncoded(
            peerIndex,
            ControlChannel,
            ENET_PACKET_FLAG_RELIABLE,
            rejectedEnvelope,
            rejection);
        return;
    }

    const auto bootstrap = HostRouter->JoinClient(peerIndex, hello.playerName);
    if (!bootstrap)
    {
        rejection.reason = 1;
        rejection.serverBuildId = Settings.buildId;
        Envelope rejectedEnvelope;
        rejectedEnvelope.messageType = MessageType::JoinRejected;
        rejectedEnvelope.sessionId = SessionId;
        rejectedEnvelope.sequence = 1;
        SendEncoded(
            peerIndex,
            ControlChannel,
            ENET_PACKET_FLAG_RELIABLE,
            rejectedEnvelope,
            rejection);
        return;
    }
    {
        std::lock_guard lock(Mutex);
        PeerByIdentity.emplace(bootstrap->assignedIdentity, peerIndex);
    }

    Envelope welcomeEnvelope;
    welcomeEnvelope.messageType = MessageType::ServerWelcome;
    welcomeEnvelope.sessionId = SessionId;
    welcomeEnvelope.sequence = 1;
    if (!SendEncoded(
            peerIndex,
            ControlChannel,
            ENET_PACKET_FLAG_RELIABLE,
            welcomeEnvelope,
            bootstrap->welcome))
    {
        Fail(WorkerError::QueueFull);
        return;
    }

    Envelope membershipEnvelope;
    membershipEnvelope.messageType = MessageType::MembershipSnapshot;
    membershipEnvelope.sessionId = SessionId;
    membershipEnvelope.sequence = Adapter->NextSequence(SequenceClass::Control);
    membershipEnvelope.streamId = HostRouter->HostIdentity().streamId;
    if (!SendEncoded(
            peerIndex,
            ControlChannel,
            ENET_PACKET_FLAG_RELIABLE,
            membershipEnvelope,
            bootstrap->membership) ||
        !Worker->MarkWelcomed(peerIndex))
    {
        Fail(WorkerError::QueueFull);
        return;
    }
    CurrentState.store(RuntimeState::Lobby);
}

bool Phase0SessionRuntime::SendEncoded(
    std::size_t peerIndex,
    std::uint8_t channel,
    std::uint32_t flags,
    const Envelope& envelope,
    const Payload& payload)
{
    std::vector<std::uint8_t> packet;
    return Phase0ProtocolCodec::Encode(envelope, payload, packet) &&
        Worker->Enqueue(peerIndex, channel, flags, std::move(packet));
}

bool Phase0SessionRuntime::SendHostFrame(
    const MemberIdentity& destination,
    const CoreFrameValue& frame)
{
    std::size_t peerIndex;
    {
        std::lock_guard lock(Mutex);
        const auto peer = PeerByIdentity.find(destination);
        if (peer == PeerByIdentity.end())
            return false;
        peerIndex = peer->second;
    }
    Envelope envelope;
    envelope.messageType = MessageType::DSFrame;
    envelope.sessionId = SessionId;
    envelope.sequence = frame.sequence;
    envelope.streamId = frame.origin.streamId;
    return SendEncoded(
        peerIndex,
        DsChannel,
        ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT,
        envelope,
        ToWireFrame(frame));
}

bool Phase0SessionRuntime::SendClientFrame(CoreFrameValue frame)
{
    if (!ClientIdentity || !(frame.origin == *ClientIdentity))
        return false;
    Envelope envelope;
    envelope.messageType = MessageType::DSFrame;
    envelope.sessionId = SessionId;
    envelope.sequence = frame.sequence;
    envelope.streamId = frame.origin.streamId;
    return SendEncoded(
        0,
        DsChannel,
        ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT,
        envelope,
        ToWireFrame(frame));
}

void Phase0SessionRuntime::SendClientEndpoint(bool active)
{
    if (!ClientIdentity || SessionId == 0 || Adapter == nullptr)
        return;
    EndpointState endpoint;
    endpoint.playerId = ClientIdentity->playerId;
    endpoint.active = active;
    endpoint.generation = ClientIdentity->generation;
    endpoint.streamId = ClientIdentity->streamId;
    Envelope envelope;
    envelope.messageType = MessageType::EndpointState;
    envelope.sessionId = SessionId;
    envelope.sequence = Adapter->NextSequence(SequenceClass::Control);
    envelope.streamId = ClientIdentity->streamId;
    if (!SendEncoded(
            0,
            ControlChannel,
            ENET_PACKET_FLAG_RELIABLE,
            envelope,
            endpoint))
        Fail(WorkerError::QueueFull);
}

void Phase0SessionRuntime::SendHostMembership(
    const MembershipSnapshot&)
{
    // Phase 0 routes from the host's authoritative member table and does not
    // need post-bootstrap UI snapshots. Full fan-out remains post-gate work.
}

void Phase0SessionRuntime::Fail(WorkerError error)
{
    CurrentError.store(error);
    if (CurrentState.load() != RuntimeState::Stopping)
        CurrentState.store(RuntimeState::Failed);
    if (Adapter)
        Adapter->StopSession();
}

DSFrame Phase0SessionRuntime::ToWireFrame(const CoreFrameValue& frame)
{
    DSFrame wire;
    wire.senderPlayerId = frame.origin.playerId;
    wire.destinationPlayerId =
        frame.destination ? frame.destination->playerId : 0xFF;
    wire.frameClass = frame.frameClass;
    wire.associationId = frame.associationId;
    wire.senderGeneration = frame.origin.generation;
    wire.destinationGeneration =
        frame.destination ? frame.destination->generation : 0;
    wire.destinationStreamId =
        frame.destination ? frame.destination->streamId : 0;
    wire.emulatedTimestamp = frame.emulatedTimestamp;
    wire.replyToCommandSequence = frame.replyToCommandSequence;
    wire.frame = frame.bytes;
    return wire;
}

CoreFrameValue Phase0SessionRuntime::FromWireFrame(
    const Envelope& envelope,
    const DSFrame& frame)
{
    CoreFrameValue value;
    value.origin = MemberIdentity{
        frame.senderPlayerId,
        frame.senderGeneration,
        envelope.streamId,
    };
    if (frame.destinationPlayerId != 0xFF)
    {
        value.destination = MemberIdentity{
            frame.destinationPlayerId,
            frame.destinationGeneration,
            frame.destinationStreamId,
        };
    }
    value.frameClass = frame.frameClass;
    value.associationId = frame.associationId;
    value.sequence = envelope.sequence;
    value.replyToCommandSequence = frame.replyToCommandSequence;
    value.emulatedTimestamp = frame.emulatedTimestamp;
    value.bytes = frame.frame;
    return value;
}

}
