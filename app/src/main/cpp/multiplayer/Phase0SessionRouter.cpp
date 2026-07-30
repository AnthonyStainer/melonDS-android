#include "Phase0SessionRouter.h"
#include "Phase0TraceRecorder.h"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <utility>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/random.h>
#endif

namespace MelonDSAndroid::Multiplayer
{
namespace
{

constexpr std::size_t MaximumExchanges = 16;
constexpr std::size_t MaximumSequenceWindows = 96;

bool RevisionAtLeast(std::uint32_t candidate, std::uint32_t baseline)
{
    return candidate == baseline ||
        Phase0ProtocolCodec::IsSequenceNewer(candidate, baseline);
}

}

Phase0SessionRouter::Phase0SessionRouter(
    std::string hostName,
    std::string sessionName,
    std::string buildId,
    std::uint8_t qualifiedMaxPlayers,
    int coreReceiveTimeout,
    RelaySink relaySink,
    LocalSink localSink,
    DiagnosticSink diagnosticSink,
    Clock clock)
    : Session(RandomNonzero64()),
      Host{0, 1, RandomNonzero32()},
      SessionName(std::move(sessionName)),
      BuildId(std::move(buildId)),
      MaximumPlayers(std::clamp<std::uint8_t>(
          qualifiedMaxPlayers,
          2,
          InitialQualifiedMaxPlayers)),
      CoreReceiveTimeout(coreReceiveTimeout),
      Relay(std::move(relaySink)),
      DeliverLocal(std::move(localSink)),
      Diagnostics(std::move(diagnosticSink)),
      Now(std::move(clock))
{
    if (!Now)
        Now = std::chrono::steady_clock::now;
    Generations[0] = 1;
    Members.emplace(0, RouterMember{
        Host,
        std::move(hostName),
        false,
        std::nullopt,
    });
}

std::uint32_t Phase0SessionRouter::MembershipRevision() const
{
    std::lock_guard lock(Mutex);
    return Revision;
}

std::optional<ClientBootstrap> Phase0SessionRouter::JoinClient(
    std::size_t transportPeer,
    std::string playerName)
{
    std::lock_guard lock(Mutex);
    if (Members.size() >= MaximumPlayers || FindByPeerLocked(transportPeer))
        return std::nullopt;

    std::uint8_t playerId = 1;
    while (playerId < ProtocolMaximumPlayers &&
        Members.find(playerId) != Members.end())
        ++playerId;
    if (playerId >= ProtocolMaximumPlayers)
        return std::nullopt;

    std::uint16_t generation = static_cast<std::uint16_t>(Generations[playerId] + 1);
    if (generation == 0)
        return std::nullopt;
    Generations[playerId] = generation;
    std::uint32_t stream = 0;
    do
    {
        stream = RandomNonzero32();
    } while (std::any_of(Members.begin(), Members.end(), [stream](const auto& pair) {
        return pair.second.identity.streamId == stream;
    }));

    const MemberIdentity identity{playerId, generation, stream};
    Members.emplace(playerId, RouterMember{
        identity,
        std::move(playerName),
        false,
        transportPeer,
    });
    Revision = Phase0ProtocolCodec::NextSequence(Revision);

    ClientBootstrap bootstrap;
    bootstrap.assignedIdentity = identity;
    bootstrap.welcome.playerId = playerId;
    bootstrap.welcome.maxPlayers = MaximumPlayers;
    bootstrap.welcome.playerGeneration = generation;
    bootstrap.welcome.playerStreamId = stream;
    bootstrap.welcome.hostGeneration = Host.generation;
    bootstrap.welcome.hostStreamId = Host.streamId;
    bootstrap.welcome.membershipRevision = Revision;
    bootstrap.welcome.sessionName = SessionName;
    bootstrap.welcome.buildId = BuildId;
    bootstrap.membership = SnapshotLocked();
    return bootstrap;
}

void Phase0SessionRouter::SetEndpointActive(
    const MemberIdentity& identity,
    bool active)
{
    std::lock_guard lock(Mutex);
    auto member = Members.find(identity.playerId);
    if (member == Members.end() || !(member->second.identity == identity) ||
        member->second.endpointActive == active)
        return;
    member->second.endpointActive = active;
    Revision = Phase0ProtocolCodec::NextSequence(Revision);
    if (!active)
    {
        for (auto& exchange : Exchanges)
            exchange.second.expected.erase(identity);
    }
}

void Phase0SessionRouter::RemoveMember(const MemberIdentity& identity)
{
    std::lock_guard lock(Mutex);
    const auto member = Members.find(identity.playerId);
    if (member == Members.end() || !(member->second.identity == identity) ||
        identity == Host)
        return;
    Members.erase(member);
    Revision = Phase0ProtocolCodec::NextSequence(Revision);
    for (auto iterator = Exchanges.begin(); iterator != Exchanges.end();)
    {
        if (iterator->first.origin == identity)
        {
            iterator = Exchanges.erase(iterator);
            continue;
        }
        iterator->second.expected.erase(identity);
        ++iterator;
    }
    for (auto iterator = SequenceWindows.begin(); iterator != SequenceWindows.end();)
    {
        if (iterator->first.origin == identity)
            iterator = SequenceWindows.erase(iterator);
        else
            ++iterator;
    }
}

bool Phase0SessionRouter::SubmitHostLocal(CoreFrameValue frame)
{
    if (!(frame.origin == Host))
    {
        Diagnose(RouterDiagnostic::ForgedOrigin, frame.origin);
        return false;
    }
    return Route(std::move(frame));
}

bool Phase0SessionRouter::ReceiveFromClient(
    std::size_t transportPeer,
    CoreFrameValue frame)
{
    {
        std::lock_guard lock(Mutex);
        const auto member = FindByPeerLocked(transportPeer);
        if (!member || !(member->identity == frame.origin))
        {
            Diagnose(RouterDiagnostic::ForgedOrigin, frame.origin);
            return false;
        }
    }
    return Route(std::move(frame));
}

bool Phase0SessionRouter::IsAuthoritativelyExhausted(
    const MemberIdentity& origin,
    std::uint32_t commandSequence)
{
    std::lock_guard lock(Mutex);
    ExpireExchangesLocked();
    const auto exchange = Exchanges.find(ExchangeKey{origin, commandSequence});
    return exchange == Exchanges.end() ||
        std::includes(
            exchange->second.responded.begin(),
            exchange->second.responded.end(),
            exchange->second.expected.begin(),
            exchange->second.expected.end());
}

bool Phase0SessionRouter::Route(CoreFrameValue frame)
{
    std::vector<MemberIdentity> destinations;
    bool deliverLocal = false;
    bool valid = true;
    {
        std::lock_guard lock(Mutex);
        ExpireExchangesLocked();
        if (!IdentityIsLiveLocked(frame.origin) ||
            !ValidateSequenceLocked(frame))
            return false;
        Phase0TraceRecorder::Get().Record(
            TraceEventType::RouterAccepted,
            frame.sequence,
            frame.emulatedTimestamp,
            static_cast<std::uint32_t>(frame.bytes.size()),
            frame.origin.playerId);

        if (frame.frameClass == FrameClass::Reply)
        {
            if (!frame.destination ||
                frame.replyToCommandSequence == 0)
                valid = false;
            else
            {
                const ExchangeKey key{
                    *frame.destination,
                    frame.replyToCommandSequence,
                };
                auto exchange = Exchanges.find(key);
                if (exchange == Exchanges.end() ||
                    exchange->second.expected.find(frame.origin) ==
                        exchange->second.expected.end() ||
                    !exchange->second.responded.insert(frame.origin).second)
                {
                    valid = false;
                }
                else
                {
                    destinations.push_back(*frame.destination);
                    deliverLocal = *frame.destination == Host;
                }
            }
        }
        else
        {
            if (frame.frameClass == FrameClass::Command)
            {
                if (Exchanges.size() >= MaximumExchanges)
                {
                    Diagnose(RouterDiagnostic::RouterOverload, frame.origin);
                    return false;
                }
                Exchange exchange;
                exchange.expiresAt = Now() + ExchangeLifetime();
                for (const auto& [id, member] : Members)
                {
                    if (member.endpointActive && !(member.identity == frame.origin))
                        exchange.expected.insert(member.identity);
                }
                Exchanges.emplace(
                    ExchangeKey{frame.origin, frame.sequence},
                    std::move(exchange));
            }
            else if (frame.frameClass == FrameClass::Ack)
            {
                if (frame.replyToCommandSequence == 0)
                    valid = false;
                else
                    Exchanges.erase(ExchangeKey{
                        frame.origin,
                        frame.replyToCommandSequence,
                    });
            }

            if (valid)
            {
                for (const auto& [id, member] : Members)
                {
                    if (member.endpointActive && !(member.identity == frame.origin))
                        destinations.push_back(member.identity);
                }
                deliverLocal = std::find(destinations.begin(), destinations.end(), Host) !=
                    destinations.end();
            }
        }
    }

    if (!valid)
    {
        Diagnose(RouterDiagnostic::InvalidExchange, frame.origin);
        return false;
    }

    bool accepted = true;
    for (const MemberIdentity& destination : destinations)
    {
        if (destination == Host)
            continue;
        if (!Relay || !Relay(destination, frame))
        {
            accepted = false;
            Diagnose(RouterDiagnostic::RelayQueueDrop, destination);
            if (frame.frameClass == FrameClass::Command)
            {
                std::lock_guard lock(Mutex);
                auto exchange = Exchanges.find(ExchangeKey{
                    frame.origin,
                    frame.sequence,
                });
                if (exchange != Exchanges.end())
                    exchange->second.expected.erase(destination);
            }
        }
        else
        {
            Phase0TraceRecorder::Get().Record(
                TraceEventType::RelayEnqueue,
                frame.sequence,
                frame.emulatedTimestamp,
                static_cast<std::uint32_t>(frame.bytes.size()),
                destination.playerId);
        }
    }
    if (deliverLocal && (!DeliverLocal || !DeliverLocal(frame)))
    {
        accepted = false;
        Diagnose(RouterDiagnostic::RelayQueueDrop, Host);
        if (frame.frameClass == FrameClass::Command)
        {
            std::lock_guard lock(Mutex);
            auto exchange = Exchanges.find(ExchangeKey{
                frame.origin,
                frame.sequence,
            });
            if (exchange != Exchanges.end())
                exchange->second.expected.erase(Host);
        }
    }
    return accepted;
}

bool Phase0SessionRouter::ValidateSequenceLocked(const CoreFrameValue& frame)
{
    const SequenceKey key{frame.origin, frame.frameClass};
    auto sequence = SequenceWindows.find(key);
    if (sequence == SequenceWindows.end())
    {
        if (SequenceWindows.size() >= MaximumSequenceWindows)
        {
            Diagnose(RouterDiagnostic::RouterOverload, frame.origin);
            return false;
        }
        SequenceWindows.emplace(key, frame.sequence);
        return frame.sequence != 0;
    }
    if (!Phase0ProtocolCodec::IsSequenceNewer(frame.sequence, sequence->second))
    {
        Diagnose(RouterDiagnostic::StaleSequence, frame.origin);
        return false;
    }
    sequence->second = frame.sequence;
    return true;
}

bool Phase0SessionRouter::IdentityIsLiveLocked(
    const MemberIdentity& identity) const
{
    const auto member = Members.find(identity.playerId);
    return member != Members.end() && member->second.identity == identity;
}

std::optional<RouterMember> Phase0SessionRouter::FindByPeerLocked(
    std::size_t peer) const
{
    for (const auto& [id, member] : Members)
    {
        if (member.transportPeer && *member.transportPeer == peer)
            return member;
    }
    return std::nullopt;
}

MembershipSnapshot Phase0SessionRouter::SnapshotLocked() const
{
    MembershipSnapshot snapshot;
    snapshot.revision = Revision;
    for (const auto& [id, member] : Members)
    {
        snapshot.members.push_back(Member{
            member.identity.playerId,
            member.endpointActive,
            member.identity.generation,
            0xFFFF,
            member.identity.streamId,
            member.name,
        });
    }
    return snapshot;
}

void Phase0SessionRouter::ExpireExchangesLocked()
{
    const auto now = Now();
    for (auto iterator = Exchanges.begin(); iterator != Exchanges.end();)
    {
        if (now >= iterator->second.expiresAt)
            iterator = Exchanges.erase(iterator);
        else
            ++iterator;
    }
}

std::chrono::milliseconds Phase0SessionRouter::ExchangeLifetime() const
{
    return std::min(
        std::chrono::milliseconds(500),
        std::max(
            std::chrono::milliseconds(100),
            std::chrono::milliseconds(4 * CoreReceiveTimeout)));
}

void Phase0SessionRouter::Diagnose(
    RouterDiagnostic diagnostic,
    std::optional<MemberIdentity> identity)
{
    if (Diagnostics)
        Diagnostics(diagnostic, std::move(identity));
}

std::uint64_t Phase0SessionRouter::RandomNonzero64()
{
    std::uint64_t result = 0;
    do
    {
#if defined(__ANDROID__)
        arc4random_buf(&result, sizeof(result));
#elif defined(__linux__)
        if (getrandom(&result, sizeof(result), 0) != sizeof(result))
            result = 0;
#endif
        if (result == 0)
        {
            std::random_device random;
            result =
                (static_cast<std::uint64_t>(random()) << 32) |
                static_cast<std::uint64_t>(random());
        }
    } while (result == 0);
    return result;
}

std::uint32_t Phase0SessionRouter::RandomNonzero32()
{
    std::uint32_t result = 0;
    do
    {
#if defined(__ANDROID__)
        arc4random_buf(&result, sizeof(result));
#elif defined(__linux__)
        if (getrandom(&result, sizeof(result), 0) != sizeof(result))
            result = 0;
#endif
        if (result == 0)
            result = std::random_device{}();
    } while (result == 0);
    return result;
}

bool Phase0ClientBootstrapValidator::AcceptWelcome(
    std::uint64_t sessionId,
    const ServerWelcome& welcome)
{
    if (CurrentState != ClientBootstrapState::Joining ||
        sessionId == 0 || welcome.playerId == 0 ||
        welcome.playerGeneration == 0 || welcome.playerStreamId == 0 ||
        welcome.hostGeneration != 1 || welcome.hostStreamId == 0 ||
        welcome.membershipRevision == 0)
    {
        CurrentState = ClientBootstrapState::Failed;
        return false;
    }
    Session = sessionId;
    WelcomeRevision = welcome.membershipRevision;
    Local = MemberIdentity{
        welcome.playerId,
        welcome.playerGeneration,
        welcome.playerStreamId,
    };
    Host = MemberIdentity{0, welcome.hostGeneration, welcome.hostStreamId};
    return true;
}

bool Phase0ClientBootstrapValidator::AcceptInitialMembership(
    const MembershipSnapshot& membership)
{
    if (CurrentState != ClientBootstrapState::Joining ||
        !Local || !Host ||
        !RevisionAtLeast(membership.revision, WelcomeRevision))
    {
        CurrentState = ClientBootstrapState::Failed;
        return false;
    }
    bool foundLocal = false;
    bool foundHost = false;
    for (const Member& member : membership.members)
    {
        const MemberIdentity identity{
            member.playerId,
            member.generation,
            member.streamId,
        };
        foundLocal |= identity == *Local;
        foundHost |= identity == *Host;
    }
    if (!foundLocal || !foundHost)
    {
        CurrentState = ClientBootstrapState::Failed;
        return false;
    }
    CurrentState = ClientBootstrapState::Lobby;
    return true;
}

bool Phase0ClientBootstrapValidator::ValidateHostRelayedFrame(
    const CoreFrameValue& frame)
{
    if (CurrentState != ClientBootstrapState::Lobby ||
        !frame.origin || frame.origin.playerId >= ProtocolMaximumPlayers ||
        frame.sequence == 0)
        return false;
    const auto key = std::make_pair(frame.origin, frame.frameClass);
    auto sequence = SequenceWindows.find(key);
    if (sequence == SequenceWindows.end())
    {
        if (SequenceWindows.size() >= MaximumSequenceWindows)
            return false;
        SequenceWindows.emplace(key, frame.sequence);
        return true;
    }
    if (!Phase0ProtocolCodec::IsSequenceNewer(frame.sequence, sequence->second))
        return false;
    sequence->second = frame.sequence;
    return true;
}

}
