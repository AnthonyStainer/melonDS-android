#ifndef MELONDS_ANDROID_PHASE0_SESSION_ROUTER_H
#define MELONDS_ANDROID_PHASE0_SESSION_ROUTER_H

#include "Phase0LanCoreAdapter.h"
#include "Phase0ProtocolCodec.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MelonDSAndroid::Multiplayer
{

struct RouterMember
{
    MemberIdentity identity;
    std::string name;
    bool endpointActive = false;
    std::optional<std::size_t> transportPeer;
};

struct ClientBootstrap
{
    ServerWelcome welcome;
    MembershipSnapshot membership;
    MemberIdentity assignedIdentity;
};

enum class RouterDiagnostic
{
    RelayQueueDrop,
    RouterOverload,
    ForgedOrigin,
    StaleSequence,
    InvalidExchange,
};

class Phase0SessionRouter
{
public:
    using RelaySink =
        std::function<bool(const MemberIdentity&, const CoreFrameValue&)>;
    using LocalSink = std::function<bool(CoreFrameValue)>;
    using DiagnosticSink =
        std::function<void(RouterDiagnostic, std::optional<MemberIdentity>)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    Phase0SessionRouter(
        std::string hostName,
        std::string sessionName,
        std::string buildId,
        std::uint8_t qualifiedMaxPlayers,
        int coreReceiveTimeout,
        RelaySink relaySink,
        LocalSink localSink,
        DiagnosticSink diagnosticSink = {},
        Clock clock = std::chrono::steady_clock::now);

    std::uint64_t SessionId() const noexcept { return Session; }
    MemberIdentity HostIdentity() const noexcept { return Host; }
    std::uint32_t MembershipRevision() const;

    std::optional<ClientBootstrap> JoinClient(
        std::size_t transportPeer,
        std::string playerName);
    void SetEndpointActive(const MemberIdentity& identity, bool active);
    void RemoveMember(const MemberIdentity& identity);

    bool SubmitHostLocal(CoreFrameValue frame);
    bool ReceiveFromClient(std::size_t transportPeer, CoreFrameValue frame);
    bool IsAuthoritativelyExhausted(
        const MemberIdentity& origin,
        std::uint32_t commandSequence);

private:
    struct ExchangeKey
    {
        MemberIdentity origin;
        std::uint32_t commandSequence = 0;

        bool operator<(const ExchangeKey& other) const noexcept
        {
            if (origin < other.origin)
                return true;
            if (other.origin < origin)
                return false;
            return commandSequence < other.commandSequence;
        }
    };

    struct Exchange
    {
        std::set<MemberIdentity> expected;
        std::set<MemberIdentity> responded;
        std::chrono::steady_clock::time_point expiresAt{};
    };

    struct SequenceKey
    {
        MemberIdentity origin;
        FrameClass frameClass = FrameClass::Regular;

        bool operator<(const SequenceKey& other) const noexcept
        {
            if (origin < other.origin)
                return true;
            if (other.origin < origin)
                return false;
            return static_cast<std::uint8_t>(frameClass) <
                static_cast<std::uint8_t>(other.frameClass);
        }
    };

    bool Route(CoreFrameValue frame);
    bool ValidateSequenceLocked(const CoreFrameValue& frame);
    bool IdentityIsLiveLocked(const MemberIdentity& identity) const;
    std::optional<RouterMember> FindByPeerLocked(std::size_t peer) const;
    MembershipSnapshot SnapshotLocked() const;
    void ExpireExchangesLocked();
    std::chrono::milliseconds ExchangeLifetime() const;
    void Diagnose(
        RouterDiagnostic diagnostic,
        std::optional<MemberIdentity> identity = std::nullopt);

    static std::uint64_t RandomNonzero64();
    static std::uint32_t RandomNonzero32();

    mutable std::mutex Mutex;
    std::uint64_t Session;
    MemberIdentity Host;
    std::string SessionName;
    std::string BuildId;
    std::uint8_t MaximumPlayers;
    int CoreReceiveTimeout;
    std::uint32_t Revision = 1;
    std::array<std::uint16_t, ProtocolMaximumPlayers> Generations{};
    std::map<std::uint8_t, RouterMember> Members;
    std::map<ExchangeKey, Exchange> Exchanges;
    std::map<SequenceKey, std::uint32_t> SequenceWindows;
    RelaySink Relay;
    LocalSink DeliverLocal;
    DiagnosticSink Diagnostics;
    Clock Now;
};

enum class ClientBootstrapState
{
    Joining,
    Lobby,
    Failed,
};

class Phase0ClientBootstrapValidator
{
public:
    bool AcceptWelcome(std::uint64_t sessionId, const ServerWelcome& welcome);
    bool AcceptInitialMembership(const MembershipSnapshot& membership);
    bool ValidateHostRelayedFrame(const CoreFrameValue& frame);

    ClientBootstrapState State() const noexcept { return CurrentState; }
    std::optional<MemberIdentity> LocalIdentity() const { return Local; }
    std::optional<MemberIdentity> HostIdentity() const { return Host; }

private:
    ClientBootstrapState CurrentState = ClientBootstrapState::Joining;
    std::uint64_t Session = 0;
    std::uint32_t WelcomeRevision = 0;
    std::optional<MemberIdentity> Local;
    std::optional<MemberIdentity> Host;
    std::map<std::pair<MemberIdentity, FrameClass>, std::uint32_t> SequenceWindows;
};

}

#endif
