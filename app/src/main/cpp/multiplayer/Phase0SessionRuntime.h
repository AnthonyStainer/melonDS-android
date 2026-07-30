#ifndef MELONDS_ANDROID_PHASE0_SESSION_RUNTIME_H
#define MELONDS_ANDROID_PHASE0_SESSION_RUNTIME_H

#include "Phase0EnetWorker.h"
#include "Phase0LanCoreAdapter.h"
#include "Phase0SessionRouter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace MelonDSAndroid::Multiplayer
{

enum class RuntimeState
{
    Starting,
    Joining,
    Lobby,
    Stopping,
    Failed,
    Stopped,
};

struct RuntimeSettings
{
    WorkerSettings transport;
    std::string playerName;
    std::string sessionName;
    std::string buildId;
};

class Phase0SessionRuntime
{
public:
    static std::unique_ptr<Phase0SessionRuntime> Create(RuntimeSettings settings);
    ~Phase0SessionRuntime();

    Phase0SessionRuntime(const Phase0SessionRuntime&) = delete;
    Phase0SessionRuntime& operator=(const Phase0SessionRuntime&) = delete;

    bool Start();
    bool Stop(std::chrono::milliseconds timeout = ShutdownSlo);
    bool AwaitStopped(std::chrono::milliseconds timeout);
    RuntimeState State() const noexcept { return CurrentState.load(); }
    WorkerError Error() const noexcept { return CurrentError.load(); }

private:
    explicit Phase0SessionRuntime(RuntimeSettings settings);
    bool Initialize();
    void InstallAdapter(
        MemberIdentity identity,
        bool applicationHostLocal);
    void HandleWorkerEvent(WorkerEvent event);
    void HandleHostPacket(std::size_t peerIndex, const DecodedPacket& packet);
    void HandleClientPacket(const DecodedPacket& packet);
    void HandleClientHello(
        std::size_t peerIndex,
        const Envelope& envelope,
        const ClientHello& hello);

    bool SendEncoded(
        std::size_t peerIndex,
        std::uint8_t channel,
        std::uint32_t flags,
        const Envelope& envelope,
        const Payload& payload);
    bool SendHostFrame(
        const MemberIdentity& destination,
        const CoreFrameValue& frame);
    bool SendClientFrame(CoreFrameValue frame);
    void SendClientEndpoint(bool active);
    void SendHostMembership(const MembershipSnapshot& snapshot);
    void Fail(WorkerError error);

    static DSFrame ToWireFrame(const CoreFrameValue& frame);
    static CoreFrameValue FromWireFrame(
        const Envelope& envelope,
        const DSFrame& frame);

    RuntimeSettings Settings;
    std::unique_ptr<Phase0EnetWorker> Worker;
    std::unique_ptr<Phase0SessionRouter> HostRouter;
    Phase0ClientBootstrapValidator ClientBootstrap;
    Phase0LanCoreAdapter* Adapter = nullptr;
    std::optional<ServerWelcome> PendingWelcome;
    std::optional<MemberIdentity> ClientIdentity;
    std::optional<MemberIdentity> ApplicationHostIdentity;
    std::uint64_t SessionId = 0;
    std::map<MemberIdentity, std::size_t> PeerByIdentity;
    mutable std::mutex Mutex;
    std::atomic<RuntimeState> CurrentState{RuntimeState::Starting};
    std::atomic<WorkerError> CurrentError{WorkerError::None};
};

}

#endif
