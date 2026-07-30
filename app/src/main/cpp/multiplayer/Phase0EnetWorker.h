#ifndef MELONDS_ANDROID_PHASE0_ENET_WORKER_H
#define MELONDS_ANDROID_PHASE0_ENET_WORKER_H

#include "Phase0ProtocolCodec.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

namespace MelonDSAndroid::Multiplayer
{

constexpr std::size_t MaximumReliableBytesPerPeer = 65536;
constexpr std::size_t MaximumReliableBytesPerHost = 524288;
constexpr std::size_t MaximumWaitingDataPerPeer = 65536;
constexpr std::chrono::milliseconds MaximumServiceWait{5};
constexpr std::chrono::milliseconds PeerHelloDeadline{1500};
constexpr std::chrono::milliseconds ShutdownSlo{2000};

enum class WorkerRole
{
    Host,
    Client,
};

enum class WorkerError
{
    None,
    EnetInitializationFailed,
    HostCreationFailed,
    NetworkBindFailed,
    PortUnavailable,
    AddressInvalid,
    ConnectFailed,
    ProtocolViolation,
    PeerHelloTimeout,
    QueueFull,
    ReliableBudgetExceeded,
    WorkerFailed,
};

struct WorkerSettings
{
    WorkerRole role = WorkerRole::Client;
    std::uint64_t networkHandle = 0;
    std::uint16_t port = 7064;
    std::array<std::uint8_t, 4> remoteIpv4{};
    std::uint8_t qualifiedMaxPlayers = InitialQualifiedMaxPlayers;
};

enum class WorkerEventType
{
    Started,
    Connected,
    Packet,
    Disconnected,
    Failure,
    Stopped,
};

struct WorkerEvent
{
    WorkerEventType type = WorkerEventType::Failure;
    WorkerError error = WorkerError::None;
    std::size_t peerIndex = 0;
    std::uint8_t channel = 0;
    std::optional<DecodedPacket> packet;
};

class Phase0EnetWorker
{
public:
    using EventCallback = std::function<void(WorkerEvent)>;

    explicit Phase0EnetWorker(WorkerSettings settings, EventCallback callback);
    ~Phase0EnetWorker();

    Phase0EnetWorker(const Phase0EnetWorker&) = delete;
    Phase0EnetWorker& operator=(const Phase0EnetWorker&) = delete;

    bool Start();
    void Stop() noexcept;
    bool WaitForStopped(std::chrono::milliseconds timeout);
    void Join();

    bool Enqueue(
        std::size_t peerIndex,
        std::uint8_t channel,
        std::uint32_t enetFlags,
        std::vector<std::uint8_t> packet);
    bool MarkWelcomed(std::size_t peerIndex);

    bool IsRunning() const noexcept { return Running.load(); }
    std::size_t ReliableBytesForPeer(std::size_t peerIndex) const noexcept;
    std::size_t ReliableBytesForHost() const noexcept;

private:
    enum class CommandType
    {
        Send,
        MarkWelcomed,
    };

    struct Command
    {
        CommandType type = CommandType::Send;
        std::size_t peerIndex = 0;
        std::uint8_t channel = 0;
        std::uint32_t enetFlags = 0;
        std::uint32_t traceSequence = 0;
        std::chrono::steady_clock::time_point enqueuedAt{};
        std::vector<std::uint8_t> packet;
    };

    struct PeerRuntime
    {
        ENetPeer* peer = nullptr;
        bool connected = false;
        bool welcomed = false;
        std::chrono::steady_clock::time_point connectedAt{};
        std::atomic_size_t reliableBytes{0};
    };

    struct ReliableCharge
    {
        Phase0EnetWorker* worker = nullptr;
        std::size_t peerIndex = 0;
        std::size_t bytes = 0;
    };

    static void OnPacketReleased(ENetPacket* packet);

    void Run();
    WorkerError CreateHost();
    void DestroyHost();
    void ServiceCommands();
    void ServiceNetwork();
    void CheckHelloDeadlines();
    bool Send(const Command& command);
    void Emit(WorkerEvent event);
    std::optional<std::size_t> PeerIndex(const ENetPeer* peer) const;
    bool ReserveReliable(std::size_t peerIndex, std::size_t bytes);
    void ReleaseReliable(std::size_t peerIndex, std::size_t bytes) noexcept;

    WorkerSettings Settings;
    EventCallback Callback;

    ENetHost* Host = nullptr;
    std::vector<std::unique_ptr<PeerRuntime>> Peers;
    std::atomic_size_t HostReliableBytes{0};

    mutable std::mutex CommandMutex;
    std::condition_variable CommandCondition;
    std::deque<Command> Commands;
    std::size_t QueuedBytes = 0;

    std::thread Thread;
    std::atomic_bool StopRequested{false};
    std::atomic_bool Running{false};
    std::mutex StateMutex;
    std::condition_variable StoppedCondition;
    bool Stopped = true;
};

}

#endif
