#include "multiplayer/Phase0LanCoreAdapter.h"
#include "multiplayer/Phase0SessionRouter.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace MelonDSAndroid::Multiplayer;

namespace
{

std::vector<std::uint8_t> Frame(std::size_t size = 36)
{
    std::vector<std::uint8_t> frame(size, 0);
    const auto internalLength = static_cast<std::uint16_t>(size - 12);
    frame[10] = static_cast<std::uint8_t>(internalLength);
    frame[11] = static_cast<std::uint8_t>(internalLength >> 8);
    return frame;
}

CoreFrameValue Command(
    MemberIdentity origin,
    std::uint32_t sequence,
    std::uint64_t timestamp = 1)
{
    CoreFrameValue frame;
    frame.origin = origin;
    frame.frameClass = FrameClass::Command;
    frame.sequence = sequence;
    frame.emulatedTimestamp = timestamp;
    frame.bytes = Frame();
    return frame;
}

void TestAdapterContextsAndWakeup()
{
    const MemberIdentity local{1, 1, 101};
    const MemberIdentity firstHost{2, 1, 202};
    const MemberIdentity secondHost{3, 1, 303};
    std::vector<CoreFrameValue> sent;
    Phase0LanCoreAdapter adapter(
        local,
        false,
        [&](CoreFrameValue frame) {
            sent.push_back(std::move(frame));
            return true;
        });
    adapter.Begin(0);
    auto bytes = Frame();

    assert(adapter.SendCmd(0, bytes.data(), bytes.size(), 10) ==
        static_cast<int>(bytes.size()));
    const auto superseded = sent.back().sequence;
    assert(adapter.SendCmd(0, bytes.data(), bytes.size(), 11) ==
        static_cast<int>(bytes.size()));
    const auto current = sent.back().sequence;
    assert(current != superseded);

    CoreFrameValue staleReply;
    staleReply.origin = firstHost;
    staleReply.destination = local;
    staleReply.frameClass = FrameClass::Reply;
    staleReply.associationId = 1;
    staleReply.sequence = 1;
    staleReply.replyToCommandSequence = superseded;
    staleReply.bytes = bytes;
    assert(!adapter.Deliver(staleReply));
    staleReply.replyToCommandSequence = current;
    assert(adapter.Deliver(staleReply));

    std::array<std::uint8_t, 15 * 1024> replySlots;
    replySlots.fill(0xA5);
    assert(adapter.RecvReplies(0, replySlots.data(), 11, 1u << 1) == (1u << 1));
    assert(std::equal(bytes.begin(), bytes.end(), replySlots.begin()));
    assert(std::all_of(
        replySlots.begin() + bytes.size(),
        replySlots.end(),
        [](std::uint8_t byte) { return byte == 0; }));

    assert(adapter.Deliver(Command(firstHost, 10, 20)));
    assert(adapter.Deliver(Command(secondHost, 20, 21)));
    std::array<std::uint8_t, 0x948> receive{};
    melonDS::u64 timestamp = 0;
    assert(adapter.RecvPacket(0, receive.data(), &timestamp) ==
        static_cast<int>(bytes.size()));
    assert(timestamp == 20);
    assert(adapter.RecvPacket(0, receive.data(), &timestamp) == 0);
    assert(adapter.SendReply(0, bytes.data(), bytes.size(), 22, 1) ==
        static_cast<int>(bytes.size()));
    assert(sent.back().destination == firstHost);
    assert(sent.back().replyToCommandSequence == 10);
    assert(adapter.RecvPacket(0, receive.data(), &timestamp) ==
        static_cast<int>(bytes.size()));
    assert(timestamp == 21);

    adapter.MemberUnavailable(secondHost);
    assert(adapter.RecvHostPacket(0, receive.data(), &timestamp) == -1);

    Phase0LanCoreAdapter waiting(
        local,
        false,
        [](CoreFrameValue) { return true; });
    waiting.Begin(0);
    int waitResult = 42;
    std::thread waiter([&] {
        waitResult = waiting.RecvHostPacket(0, receive.data(), &timestamp);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    waiting.StopSession();
    waiter.join();
    assert(waitResult == -1);

    bytes[10] ^= 1;
    assert(adapter.SendPacket(0, bytes.data(), bytes.size(), 0) == 0);
}

void TestClientReplyWaitDoesNotTrustLocalExhaustion()
{
    const MemberIdentity local{1, 1, 101};
    Phase0LanCoreAdapter adapter(
        local,
        false,
        [](CoreFrameValue) { return true; },
        {},
        [](const MemberIdentity&, std::uint32_t) {
            return true;
        });
    adapter.SetRecvTimeout(20);
    adapter.Begin(0);
    auto bytes = Frame();
    assert(adapter.SendCmd(0, bytes.data(), bytes.size(), 1) ==
        static_cast<int>(bytes.size()));
    std::array<std::uint8_t, 15 * 1024> replies{};
    const auto started = std::chrono::steady_clock::now();
    assert(adapter.RecvReplies(0, replies.data(), 1, 1u << 2) == 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(elapsed >= std::chrono::milliseconds(15));
}

void TestRouterRelayIdentityExchangeAndBootstrap()
{
    std::vector<std::pair<MemberIdentity, CoreFrameValue>> relayed;
    std::vector<CoreFrameValue> localDelivery;
    Phase0SessionRouter router(
        "Host",
        "Phase 0",
        "test",
        4,
        25,
        [&](const MemberIdentity& destination, const CoreFrameValue& frame) {
            relayed.emplace_back(destination, frame);
            return true;
        },
        [&](CoreFrameValue frame) {
            localDelivery.push_back(std::move(frame));
            return true;
        });
    const MemberIdentity host = router.HostIdentity();
    assert(host.playerId == 0 && host.generation == 1 && host.streamId != 0);
    assert(router.SessionId() != 0);

    const auto first = router.JoinClient(0, "A");
    const auto second = router.JoinClient(1, "B");
    assert(first && second);
    assert(first->welcome.hostStreamId == host.streamId);
    assert(first->membership.members.front().playerId == 0);
    router.SetEndpointActive(host, true);
    router.SetEndpointActive(first->assignedIdentity, true);
    router.SetEndpointActive(second->assignedIdentity, true);

    CoreFrameValue command = Command(first->assignedIdentity, 1);
    assert(router.ReceiveFromClient(0, command));
    assert(localDelivery.size() == 1);
    assert(localDelivery.front().origin == first->assignedIdentity);
    assert(std::any_of(relayed.begin(), relayed.end(), [&](const auto& item) {
        return item.first == second->assignedIdentity &&
            item.second.origin == first->assignedIdentity;
    }));

    CoreFrameValue reply;
    reply.origin = second->assignedIdentity;
    reply.destination = first->assignedIdentity;
    reply.frameClass = FrameClass::Reply;
    reply.associationId = 2;
    reply.sequence = 1;
    reply.replyToCommandSequence = command.sequence;
    reply.bytes = Frame();
    assert(router.ReceiveFromClient(1, reply));
    assert(relayed.back().first == first->assignedIdentity);
    assert(relayed.back().second.origin == second->assignedIdentity);
    assert(!router.IsAuthoritativelyExhausted(
        first->assignedIdentity,
        command.sequence));
    router.SetEndpointActive(host, false);
    assert(router.IsAuthoritativelyExhausted(
        first->assignedIdentity,
        command.sequence));

    CoreFrameValue forged = Command(second->assignedIdentity, 2);
    assert(!router.ReceiveFromClient(0, forged));
    assert(!router.ReceiveFromClient(1, reply));

    Phase0ClientBootstrapValidator validator;
    assert(validator.AcceptWelcome(router.SessionId(), first->welcome));
    assert(validator.State() == ClientBootstrapState::Joining);
    assert(validator.AcceptInitialMembership(first->membership));
    assert(validator.State() == ClientBootstrapState::Lobby);

    const auto third = router.JoinClient(2, "C");
    assert(third);
    CoreFrameValue beforeMembership = Command(third->assignedIdentity, 1);
    assert(validator.ValidateHostRelayedFrame(beforeMembership));

    Phase0ClientBootstrapValidator disagreement;
    assert(disagreement.AcceptWelcome(router.SessionId(), second->welcome));
    auto badSnapshot = second->membership;
    badSnapshot.members.front().streamId ^= 1;
    assert(!disagreement.AcceptInitialMembership(badSnapshot));
    assert(disagreement.State() == ClientBootstrapState::Failed);

    router.RemoveMember(second->assignedIdentity);
    assert(router.IsAuthoritativelyExhausted(
        first->assignedIdentity,
        command.sequence));
}

}

int main()
{
    TestAdapterContextsAndWakeup();
    TestClientReplyWaitDoesNotTrustLocalExhaustion();
    TestRouterRelayIdentityExchangeAndBootstrap();
    std::cout << "Phase0CoreRouterTest passed\n";
    return 0;
}
