#include "Phase0EnetWorker.h"
#include "Phase0TraceRecorder.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <utility>

#ifdef __ANDROID__
#include <android/multinetwork.h>
#endif

namespace MelonDSAndroid::Multiplayer
{
namespace
{

constexpr std::size_t MaximumQueuedPackets = 256;
constexpr std::size_t MaximumQueuedBytes = 512 * 1024;
constexpr std::uint8_t ChannelCount = 2;
constexpr std::uint8_t ControlChannel = 0;
constexpr std::uint8_t DsChannel = 1;

}

Phase0EnetWorker::Phase0EnetWorker(
    WorkerSettings settings,
    EventCallback callback)
    : Settings(settings), Callback(std::move(callback))
{
    if (Settings.qualifiedMaxPlayers < 2 ||
        Settings.qualifiedMaxPlayers > InitialQualifiedMaxPlayers)
        Settings.qualifiedMaxPlayers = InitialQualifiedMaxPlayers;
}

Phase0EnetWorker::~Phase0EnetWorker()
{
    Stop();
    Join();
}

bool Phase0EnetWorker::Start()
{
    std::lock_guard stateLock(StateMutex);
    if (!Stopped || Thread.joinable())
        return false;
    StopRequested.store(false);
    Stopped = false;
    Thread = std::thread(&Phase0EnetWorker::Run, this);
    return true;
}

void Phase0EnetWorker::Stop() noexcept
{
    StopRequested.store(true);
    CommandCondition.notify_all();
}

bool Phase0EnetWorker::WaitForStopped(std::chrono::milliseconds timeout)
{
    std::unique_lock stateLock(StateMutex);
    return StoppedCondition.wait_for(stateLock, timeout, [this] { return Stopped; });
}

void Phase0EnetWorker::Join()
{
    if (Thread.joinable())
        Thread.join();
}

bool Phase0EnetWorker::Enqueue(
    std::size_t peerIndex,
    std::uint8_t channel,
    std::uint32_t enetFlags,
    std::vector<std::uint8_t> packet)
{
    if (StopRequested.load() ||
        peerIndex >= Peers.size() ||
        channel >= ChannelCount ||
        packet.empty() ||
        packet.size() > MaximumApplicationPacketSize)
        return false;

    std::lock_guard commandLock(CommandMutex);
    if (Commands.size() >= MaximumQueuedPackets ||
        QueuedBytes + packet.size() > MaximumQueuedBytes)
        return false;
    QueuedBytes += packet.size();
    const DecodeResult decoded = Phase0ProtocolCodec::Decode(
        packet.data(),
        packet.size());
    const std::uint32_t traceSequence =
        decoded ? decoded.packet.envelope.sequence : 0;
    Phase0TraceRecorder::Get().Record(
        TraceEventType::CoreToWorkerEnqueue,
        traceSequence,
        0,
        static_cast<std::uint32_t>(Commands.size()),
        0,
        static_cast<std::uint8_t>(peerIndex));
    Commands.push_back(Command{
        CommandType::Send,
        peerIndex,
        channel,
        enetFlags,
        traceSequence,
        std::chrono::steady_clock::now(),
        std::move(packet),
    });
    CommandCondition.notify_one();
    return true;
}

bool Phase0EnetWorker::MarkWelcomed(std::size_t peerIndex)
{
    if (StopRequested.load() || peerIndex >= Peers.size())
        return false;
    std::lock_guard commandLock(CommandMutex);
    if (Commands.size() >= MaximumQueuedPackets)
        return false;
    Commands.push_back(Command{
        CommandType::MarkWelcomed,
        peerIndex,
        0,
        0,
        0,
        std::chrono::steady_clock::now(),
        {},
    });
    CommandCondition.notify_one();
    return true;
}

std::size_t Phase0EnetWorker::ReliableBytesForPeer(std::size_t peerIndex) const noexcept
{
    if (peerIndex >= Peers.size())
        return 0;
    return Peers[peerIndex]->reliableBytes.load();
}

std::size_t Phase0EnetWorker::ReliableBytesForHost() const noexcept
{
    return HostReliableBytes.load();
}

void Phase0EnetWorker::OnPacketReleased(ENetPacket* packet)
{
    auto* charge = static_cast<ReliableCharge*>(packet->userData);
    if (charge != nullptr)
    {
        charge->worker->ReleaseReliable(charge->peerIndex, charge->bytes);
        delete charge;
        packet->userData = nullptr;
    }
}

void Phase0EnetWorker::Run()
{
    WorkerError error = WorkerError::None;
    if (enet_initialize() != 0)
        error = WorkerError::EnetInitializationFailed;
    else
        error = CreateHost();

    if (error != WorkerError::None)
    {
        Emit(WorkerEvent{WorkerEventType::Failure, error});
    }
    else
    {
        Running.store(true);
        Emit(WorkerEvent{WorkerEventType::Started});
        while (!StopRequested.load())
        {
            ServiceCommands();
            ServiceNetwork();
            CheckHelloDeadlines();

            std::unique_lock commandLock(CommandMutex);
            if (Commands.empty() && !StopRequested.load())
                CommandCondition.wait_for(commandLock, MaximumServiceWait);
        }
    }

    Running.store(false);
    DestroyHost();
    if (error != WorkerError::EnetInitializationFailed)
        enet_deinitialize();

    {
        std::lock_guard stateLock(StateMutex);
        Stopped = true;
    }
    StoppedCondition.notify_all();
    Emit(WorkerEvent{WorkerEventType::Stopped});
}

WorkerError Phase0EnetWorker::CreateHost()
{
    const std::size_t peerCount = Settings.role == WorkerRole::Host
        ? Settings.qualifiedMaxPlayers - 1
        : 1;
    Host = enet_host_create(nullptr, peerCount, ChannelCount, 0, 0);
    if (Host == nullptr)
        return WorkerError::HostCreationFailed;

    Host->maximumPacketSize = MaximumApplicationPacketSize;
    Host->maximumWaitingData = MaximumWaitingDataPerPeer;
    Peers.reserve(peerCount);
    for (std::size_t index = 0; index < peerCount; ++index)
        Peers.push_back(std::make_unique<PeerRuntime>());

#ifdef __ANDROID__
    if (Settings.networkHandle == 0 ||
        android_setsocknetwork(
            static_cast<net_handle_t>(Settings.networkHandle),
            Host->socket) != 0)
        return WorkerError::NetworkBindFailed;
#else
    if (Settings.networkHandle != 0)
        return WorkerError::NetworkBindFailed;
#endif

    if (Settings.role == WorkerRole::Host)
    {
        ENetAddress bindAddress{};
        bindAddress.host = ENET_HOST_ANY;
        bindAddress.port = Settings.port;
        if (enet_socket_bind(Host->socket, &bindAddress) != 0)
            return errno == EADDRINUSE
                ? WorkerError::PortUnavailable
                : WorkerError::NetworkBindFailed;
        if (enet_socket_get_address(Host->socket, &Host->address) != 0 ||
            Host->address.port != Settings.port)
            return WorkerError::NetworkBindFailed;
        return WorkerError::None;
    }

    char addressText[16];
    const int length = std::snprintf(
        addressText,
        sizeof(addressText),
        "%u.%u.%u.%u",
        Settings.remoteIpv4[0],
        Settings.remoteIpv4[1],
        Settings.remoteIpv4[2],
        Settings.remoteIpv4[3]);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(addressText))
        return WorkerError::AddressInvalid;

    ENetAddress address{};
    address.port = Settings.port;
    if (enet_address_set_host_ip(&address, addressText) != 0)
        return WorkerError::AddressInvalid;
    ENetPeer* peer = enet_host_connect(Host, &address, ChannelCount, 0);
    if (peer == nullptr)
        return WorkerError::ConnectFailed;
    Peers[0]->peer = peer;
    return WorkerError::None;
}

void Phase0EnetWorker::DestroyHost()
{
    {
        std::lock_guard commandLock(CommandMutex);
        Commands.clear();
        QueuedBytes = 0;
    }
    if (Host != nullptr)
    {
        for (auto& peer : Peers)
        {
            if (peer->peer != nullptr)
                enet_peer_reset(peer->peer);
            peer->peer = nullptr;
            peer->connected = false;
        }
        enet_host_destroy(Host);
        Host = nullptr;
    }
    Peers.clear();
}

void Phase0EnetWorker::ServiceCommands()
{
    std::deque<Command> commands;
    {
        std::lock_guard commandLock(CommandMutex);
        commands.swap(Commands);
        QueuedBytes = 0;
    }
    for (const Command& command : commands)
    {
        const auto residence = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - command.enqueuedAt).count();
        Phase0TraceRecorder::Get().Record(
            TraceEventType::WorkerCommandDequeue,
            command.traceSequence,
            0,
            static_cast<std::uint32_t>(std::min<std::int64_t>(
                residence,
                UINT32_MAX)),
            0,
            static_cast<std::uint8_t>(command.peerIndex));
        if (command.peerIndex >= Peers.size())
            continue;
        if (command.type == CommandType::MarkWelcomed)
        {
            Peers[command.peerIndex]->welcomed = true;
            continue;
        }
        if (!Send(command))
            Emit(WorkerEvent{
                WorkerEventType::Failure,
                WorkerError::ReliableBudgetExceeded,
                command.peerIndex,
                command.channel,
            });
    }
}

void Phase0EnetWorker::ServiceNetwork()
{
    ENetEvent event{};
    while (!StopRequested.load() && enet_host_service(Host, &event, 0) > 0)
    {
        Phase0TraceRecorder::Get().Record(TraceEventType::EnetService);
        const auto index = PeerIndex(event.peer);
        if (!index)
        {
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
            continue;
        }
        PeerRuntime& runtime = *Peers[*index];
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            runtime.peer = event.peer;
            runtime.connected = true;
            runtime.welcomed = Settings.role == WorkerRole::Client;
            runtime.connectedAt = std::chrono::steady_clock::now();
            enet_peer_ping_interval(event.peer, 500);
            enet_peer_timeout(event.peer, 4, 1000, 5000);
            Emit(WorkerEvent{
                WorkerEventType::Connected,
                WorkerError::None,
                *index,
            });
            break;
        case ENET_EVENT_TYPE_RECEIVE:
        {
            const std::size_t receivedLength = event.packet->dataLength;
            const DecodeResult decoded = Phase0ProtocolCodec::Decode(
                event.packet->data,
                receivedLength);
            enet_packet_destroy(event.packet);
            if (!decoded)
            {
                Emit(WorkerEvent{
                    WorkerEventType::Failure,
                    WorkerError::ProtocolViolation,
                    *index,
                    event.channelID,
                });
                break;
            }
            WorkerEvent workerEvent{
                WorkerEventType::Packet,
                WorkerError::None,
                *index,
                event.channelID,
            };
            workerEvent.packet = decoded.packet;
            Phase0TraceRecorder::Get().Record(
                TraceEventType::TransportIngress,
                decoded.packet.envelope.sequence,
                0,
                static_cast<std::uint32_t>(receivedLength),
                0,
                static_cast<std::uint8_t>(*index));
            Emit(std::move(workerEvent));
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            runtime.connected = false;
            runtime.welcomed = false;
            runtime.peer = nullptr;
            Emit(WorkerEvent{
                WorkerEventType::Disconnected,
                WorkerError::None,
                *index,
            });
            break;
        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }
}

void Phase0EnetWorker::CheckHelloDeadlines()
{
    if (Settings.role != WorkerRole::Host)
        return;
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < Peers.size(); ++index)
    {
        PeerRuntime& peer = *Peers[index];
        if (peer.connected && !peer.welcomed &&
            now - peer.connectedAt >= PeerHelloDeadline)
        {
            enet_peer_disconnect_now(peer.peer, 0);
            peer.connected = false;
            peer.peer = nullptr;
            Emit(WorkerEvent{
                WorkerEventType::Failure,
                WorkerError::PeerHelloTimeout,
                index,
            });
        }
    }
}

bool Phase0EnetWorker::Send(const Command& command)
{
    PeerRuntime& runtime = *Peers[command.peerIndex];
    if (!runtime.connected || runtime.peer == nullptr)
        return false;

    const bool reliable = (command.enetFlags & ENET_PACKET_FLAG_RELIABLE) != 0;
    if (reliable && !ReserveReliable(command.peerIndex, command.packet.size()))
        return false;

    ENetPacket* packet = enet_packet_create(
        command.packet.data(),
        command.packet.size(),
        command.enetFlags);
    if (packet == nullptr)
    {
        if (reliable)
            ReleaseReliable(command.peerIndex, command.packet.size());
        return false;
    }
    if (reliable)
    {
        packet->userData = new ReliableCharge{
            this,
            command.peerIndex,
            command.packet.size(),
        };
        packet->freeCallback = &Phase0EnetWorker::OnPacketReleased;
    }
    if (enet_peer_send(runtime.peer, command.channel, packet) != 0)
    {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

void Phase0EnetWorker::Emit(WorkerEvent event)
{
    if (Callback)
        Callback(std::move(event));
}

std::optional<std::size_t> Phase0EnetWorker::PeerIndex(const ENetPeer* peer) const
{
    if (peer == nullptr || Host == nullptr ||
        peer < Host->peers || peer >= Host->peers + Host->peerCount)
        return std::nullopt;
    const auto index = static_cast<std::size_t>(peer - Host->peers);
    if (index >= Peers.size())
        return std::nullopt;
    return index;
}

bool Phase0EnetWorker::ReserveReliable(std::size_t peerIndex, std::size_t bytes)
{
    if (peerIndex >= Peers.size() || bytes > MaximumReliableBytesPerPeer)
        return false;
    auto& peerBytes = Peers[peerIndex]->reliableBytes;
    std::size_t currentPeer = peerBytes.load();
    while (true)
    {
        if (currentPeer + bytes > MaximumReliableBytesPerPeer)
            return false;
        if (peerBytes.compare_exchange_weak(currentPeer, currentPeer + bytes))
            break;
    }

    std::size_t currentHost = HostReliableBytes.load();
    while (true)
    {
        if (currentHost + bytes > MaximumReliableBytesPerHost)
        {
            peerBytes.fetch_sub(bytes);
            return false;
        }
        if (HostReliableBytes.compare_exchange_weak(currentHost, currentHost + bytes))
            return true;
    }
}

void Phase0EnetWorker::ReleaseReliable(
    std::size_t peerIndex,
    std::size_t bytes) noexcept
{
    if (peerIndex < Peers.size())
        Peers[peerIndex]->reliableBytes.fetch_sub(bytes);
    HostReliableBytes.fetch_sub(bytes);
}

}
