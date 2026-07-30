#include "multiplayer/Phase0ProtocolCodec.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <variant>
#include <vector>

using namespace MelonDSAndroid::Multiplayer;

namespace
{

std::vector<std::uint8_t> CoreFrame(std::size_t size)
{
    std::vector<std::uint8_t> frame(size, 0);
    const auto bodyLength = static_cast<std::uint16_t>(size - 12);
    frame[10] = static_cast<std::uint8_t>(bodyLength);
    frame[11] = static_cast<std::uint8_t>(bodyLength >> 8);
    return frame;
}

void TestHelloRoundTrip()
{
    Envelope envelope;
    envelope.messageType = MessageType::ClientHello;
    envelope.sequence = 1;
    ClientHello hello;
    hello.playerName = "Android A";
    hello.buildId = "phase0-test";

    std::vector<std::uint8_t> encoded;
    assert(Phase0ProtocolCodec::Encode(envelope, hello, encoded));
    assert(encoded.size() == EnvelopeSize + 24 + 9 + 11);
    assert(encoded[0] == 'M' && encoded[1] == 'L' &&
        encoded[2] == 'M' && encoded[3] == 'P');

    const DecodeResult decoded = Phase0ProtocolCodec::Decode(encoded.data(), encoded.size());
    assert(decoded);
    const auto& parsed = std::get<ClientHello>(decoded.packet.payload);
    assert(parsed.playerName == hello.playerName);
    assert(parsed.buildId == hello.buildId);

    encoded[14] = 1;
    assert(Phase0ProtocolCodec::Decode(encoded.data(), encoded.size()).error ==
        DecodeError::NonzeroReserved);
}

void TestWelcomeAndMembershipRoundTrip()
{
    Envelope welcomeEnvelope;
    welcomeEnvelope.messageType = MessageType::ServerWelcome;
    welcomeEnvelope.sessionId = 0x0102030405060708ULL;
    welcomeEnvelope.sequence = 1;
    ServerWelcome welcome;
    welcome.playerId = 1;
    welcome.playerGeneration = 1;
    welcome.playerStreamId = 0x11223344;
    welcome.hostStreamId = 0x55667788;
    welcome.membershipRevision = 2;
    welcome.sessionName = "Phase 0";
    welcome.buildId = "test";
    std::vector<std::uint8_t> encoded;
    assert(Phase0ProtocolCodec::Encode(welcomeEnvelope, welcome, encoded));
    assert(Phase0ProtocolCodec::Decode(encoded.data(), encoded.size()));

    Envelope snapshotEnvelope;
    snapshotEnvelope.messageType = MessageType::MembershipSnapshot;
    snapshotEnvelope.sessionId = welcomeEnvelope.sessionId;
    snapshotEnvelope.sequence = 1;
    snapshotEnvelope.streamId = welcome.hostStreamId;
    MembershipSnapshot snapshot;
    snapshot.revision = 2;
    snapshot.members = {
        Member{0, true, 1, 1, welcome.hostStreamId, "Host"},
        Member{1, true, 1, 1, welcome.playerStreamId, "Client"},
    };
    assert(Phase0ProtocolCodec::Encode(snapshotEnvelope, snapshot, encoded));
    const DecodeResult decoded = Phase0ProtocolCodec::Decode(encoded.data(), encoded.size());
    assert(decoded);
    assert(std::get<MembershipSnapshot>(decoded.packet.payload).members.size() == 2);
}

void TestFrames()
{
    Envelope envelope;
    envelope.messageType = MessageType::DSFrame;
    envelope.sessionId = 1;
    envelope.sequence = 1;
    envelope.streamId = 2;
    DSFrame command;
    command.senderPlayerId = 1;
    command.senderGeneration = 1;
    command.frameClass = FrameClass::Command;
    command.frame = CoreFrame(2376);
    std::vector<std::uint8_t> encoded;
    assert(Phase0ProtocolCodec::Encode(envelope, command, encoded));
    assert(encoded.size() == EnvelopeSize + 28 + 2376);
    assert(Phase0ProtocolCodec::Decode(encoded.data(), encoded.size()));

    command.frame[10] ^= 1;
    assert(!Phase0ProtocolCodec::Encode(envelope, command, encoded));

    DSFrame defaultReply;
    defaultReply.senderPlayerId = 2;
    defaultReply.destinationPlayerId = 1;
    defaultReply.frameClass = FrameClass::Reply;
    defaultReply.senderGeneration = 1;
    defaultReply.destinationGeneration = 1;
    defaultReply.destinationStreamId = 2;
    defaultReply.replyToCommandSequence = 1;
    assert(Phase0ProtocolCodec::Encode(envelope, defaultReply, encoded));
}

void TestSequences()
{
    assert(Phase0ProtocolCodec::NextSequence(0) == 1);
    assert(Phase0ProtocolCodec::NextSequence(UINT32_MAX) == 1);
    assert(Phase0ProtocolCodec::IsSequenceNewer(1, UINT32_MAX));
    assert(!Phase0ProtocolCodec::IsSequenceNewer(UINT32_MAX, 1));
}

}

int main()
{
    TestHelloRoundTrip();
    TestWelcomeAndMembershipRoundTrip();
    TestFrames();
    TestSequences();
    std::cout << "Phase0ProtocolCodecTest passed\n";
    return 0;
}
