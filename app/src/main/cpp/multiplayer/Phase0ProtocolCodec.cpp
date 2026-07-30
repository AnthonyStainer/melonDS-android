#include "Phase0ProtocolCodec.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace MelonDSAndroid::Multiplayer
{
namespace
{

constexpr std::uint8_t Magic[4] = {'M', 'L', 'M', 'P'};

void Put16(std::vector<std::uint8_t>& value, std::size_t offset, std::uint16_t field)
{
    value[offset] = static_cast<std::uint8_t>(field);
    value[offset + 1] = static_cast<std::uint8_t>(field >> 8);
}

void Put32(std::vector<std::uint8_t>& value, std::size_t offset, std::uint32_t field)
{
    for (std::size_t i = 0; i < 4; ++i)
        value[offset + i] = static_cast<std::uint8_t>(field >> (i * 8));
}

void Put64(std::vector<std::uint8_t>& value, std::size_t offset, std::uint64_t field)
{
    for (std::size_t i = 0; i < 8; ++i)
        value[offset + i] = static_cast<std::uint8_t>(field >> (i * 8));
}

std::uint16_t Get16(const std::uint8_t* value, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        value[offset] | (static_cast<std::uint16_t>(value[offset + 1]) << 8));
}

std::uint32_t Get32(const std::uint8_t* value, std::size_t offset)
{
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < 4; ++i)
        result |= static_cast<std::uint32_t>(value[offset + i]) << (i * 8);
    return result;
}

std::uint64_t Get64(const std::uint8_t* value, std::size_t offset)
{
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i)
        result |= static_cast<std::uint64_t>(value[offset + i]) << (i * 8);
    return result;
}

bool IsKnownMessage(std::uint8_t type)
{
    switch (static_cast<MessageType>(type))
    {
    case MessageType::ClientHello:
    case MessageType::ServerWelcome:
    case MessageType::JoinRejected:
    case MessageType::MembershipSnapshot:
    case MessageType::EndpointState:
    case MessageType::DSFrame:
    case MessageType::ChannelBarrier:
        return true;
    }
    return false;
}

bool IsValidBuildId(const std::string& value)
{
    if (value.size() > 64)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20 && byte <= 0x7E;
    });
}

bool DecodeCodePoint(
    const std::string& value,
    std::size_t& offset,
    std::uint32_t& codePoint)
{
    const auto first = static_cast<std::uint8_t>(value[offset]);
    std::size_t continuationCount = 0;
    std::uint32_t minimum = 0;

    if (first <= 0x7F)
    {
        codePoint = first;
        ++offset;
        return true;
    }
    if ((first & 0xE0) == 0xC0)
    {
        codePoint = first & 0x1F;
        continuationCount = 1;
        minimum = 0x80;
    }
    else if ((first & 0xF0) == 0xE0)
    {
        codePoint = first & 0x0F;
        continuationCount = 2;
        minimum = 0x800;
    }
    else if ((first & 0xF8) == 0xF0)
    {
        codePoint = first & 0x07;
        continuationCount = 3;
        minimum = 0x10000;
    }
    else
    {
        return false;
    }

    if (offset + continuationCount >= value.size())
        return false;
    for (std::size_t index = 1; index <= continuationCount; ++index)
    {
        const auto byte = static_cast<std::uint8_t>(value[offset + index]);
        if ((byte & 0xC0) != 0x80)
            return false;
        codePoint = (codePoint << 6) | (byte & 0x3F);
    }
    offset += continuationCount + 1;
    return codePoint >= minimum &&
        codePoint <= 0x10FFFF &&
        !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
}

bool IsValidName(const std::string& value, std::size_t maximumBytes)
{
    if (value.empty() || value.size() > maximumBytes)
        return false;

    std::size_t offset = 0;
    while (offset < value.size())
    {
        std::uint32_t codePoint = 0;
        if (!DecodeCodePoint(value, offset, codePoint))
            return false;
        if (codePoint == 0 ||
            (codePoint >= 0x01 && codePoint <= 0x1F) ||
            (codePoint >= 0x7F && codePoint <= 0x9F) ||
            (codePoint >= 0x202A && codePoint <= 0x202E) ||
            (codePoint >= 0x2066 && codePoint <= 0x2069))
            return false;
    }
    return true;
}

bool IsValidFrame(const DSFrame& frame)
{
    const auto size = frame.frame.size();
    if (frame.senderPlayerId >= ProtocolMaximumPlayers ||
        static_cast<std::uint8_t>(frame.frameClass) > 3 ||
        frame.senderGeneration == 0)
        return false;

    if (frame.frameClass == FrameClass::Reply)
    {
        if (size == 0)
            return frame.associationId == 0 &&
                frame.destinationPlayerId < ProtocolMaximumPlayers &&
                frame.destinationGeneration != 0 &&
                frame.destinationStreamId != 0 &&
                frame.replyToCommandSequence != 0;
        if (frame.associationId == 0 || frame.associationId > 15 ||
            size < 36 || size > 1024)
            return false;
    }
    else
    {
        if (size < 36 || size > 0x948 || frame.associationId != 0)
            return false;
    }

    if (size < 12 || Get16(frame.frame.data(), 10) != size - 12)
        return false;

    if (frame.frameClass == FrameClass::Regular ||
        frame.frameClass == FrameClass::Command ||
        frame.frameClass == FrameClass::Ack)
    {
        if (frame.destinationPlayerId != 0xFF ||
            frame.destinationGeneration != 0 ||
            frame.destinationStreamId != 0)
            return false;
    }
    if (frame.frameClass == FrameClass::Regular &&
        frame.replyToCommandSequence != 0)
        return false;
    if (frame.frameClass == FrameClass::Command &&
        frame.replyToCommandSequence != 0)
        return false;
    if (frame.frameClass == FrameClass::Ack &&
        frame.replyToCommandSequence == 0)
        return false;
    return true;
}

bool BuildPayload(const ClientHello& value, std::vector<std::uint8_t>& payload)
{
    if (!IsValidName(value.playerName, 31) ||
        !IsValidBuildId(value.buildId) ||
        value.coreNetworkEpoch == 0)
        return false;
    payload.assign(24 + value.playerName.size() + value.buildId.size(), 0);
    Put32(payload, 0, value.coreNetworkEpoch);
    payload[4] = static_cast<std::uint8_t>(value.playerName.size());
    payload[5] = static_cast<std::uint8_t>(value.buildId.size());
    Put64(payload, 8, value.supportedFeatures);
    Put64(payload, 16, value.requiredFeatures);
    std::copy(value.playerName.begin(), value.playerName.end(), payload.begin() + 24);
    std::copy(value.buildId.begin(), value.buildId.end(), payload.begin() + 24 + value.playerName.size());
    return true;
}

bool BuildPayload(const ServerWelcome& value, std::vector<std::uint8_t>& payload)
{
    if (value.playerId == 0 || value.playerId >= ProtocolMaximumPlayers ||
        value.maxPlayers < 2 || value.maxPlayers > InitialQualifiedMaxPlayers ||
        value.playerGeneration == 0 || value.playerStreamId == 0 ||
        value.hostGeneration != 1 || value.hostStreamId == 0 ||
        value.membershipRevision == 0 || value.coreNetworkEpoch == 0 ||
        !IsValidName(value.sessionName, 63) || !IsValidBuildId(value.buildId))
        return false;
    payload.assign(44 + value.sessionName.size() + value.buildId.size(), 0);
    payload[0] = value.playerId;
    payload[1] = value.maxPlayers;
    Put16(payload, 2, value.playerGeneration);
    Put32(payload, 4, value.playerStreamId);
    payload[8] = 0;
    Put16(payload, 10, value.hostGeneration);
    Put32(payload, 12, value.hostStreamId);
    Put32(payload, 16, value.membershipRevision);
    Put32(payload, 20, value.coreNetworkEpoch);
    Put64(payload, 24, value.supportedFeatures);
    Put64(payload, 32, value.requiredFeatures);
    payload[40] = static_cast<std::uint8_t>(value.sessionName.size());
    payload[41] = static_cast<std::uint8_t>(value.buildId.size());
    std::copy(value.sessionName.begin(), value.sessionName.end(), payload.begin() + 44);
    std::copy(value.buildId.begin(), value.buildId.end(), payload.begin() + 44 + value.sessionName.size());
    return true;
}

bool BuildPayload(const JoinRejected& value, std::vector<std::uint8_t>& payload)
{
    if (value.reason < 1 || value.reason > 11 ||
        value.serverProtocolMajor == 0 ||
        value.serverCoreNetworkEpoch == 0 ||
        !IsValidBuildId(value.serverBuildId))
        return false;
    payload.assign(20 + value.serverBuildId.size(), 0);
    Put16(payload, 0, value.reason);
    payload[2] = value.serverProtocolMajor;
    payload[3] = value.serverProtocolMinor;
    Put32(payload, 4, value.serverCoreNetworkEpoch);
    Put64(payload, 8, value.serverRequiredFeatures);
    payload[16] = static_cast<std::uint8_t>(value.serverBuildId.size());
    std::copy(value.serverBuildId.begin(), value.serverBuildId.end(), payload.begin() + 20);
    return true;
}

bool BuildPayload(const MembershipSnapshot& value, std::vector<std::uint8_t>& payload)
{
    if (value.revision == 0 || value.members.empty() ||
        value.members.size() > ProtocolMaximumPlayers)
        return false;

    std::uint8_t priorId = 0;
    bool first = true;
    std::size_t payloadSize = 8;
    for (const Member& member : value.members)
    {
        if (member.playerId >= ProtocolMaximumPlayers ||
            member.generation == 0 || member.streamId == 0 ||
            !IsValidName(member.playerName, 31) ||
            (!first && member.playerId <= priorId))
            return false;
        payloadSize += 16 + member.playerName.size();
        priorId = member.playerId;
        first = false;
    }
    if (payloadSize + EnvelopeSize > MaximumApplicationPacketSize)
        return false;

    payload.assign(payloadSize, 0);
    Put32(payload, 0, value.revision);
    payload[4] = value.locked ? 1 : 0;
    payload[5] = static_cast<std::uint8_t>(value.members.size());
    std::size_t offset = 8;
    for (const Member& member : value.members)
    {
        Put16(payload, offset, static_cast<std::uint16_t>(16 + member.playerName.size()));
        payload[offset + 2] = member.playerId;
        payload[offset + 3] = member.endpointActive ? 1 : 0;
        Put16(payload, offset + 4, member.generation);
        Put16(payload, offset + 6, member.pingMilliseconds);
        Put32(payload, offset + 8, member.streamId);
        payload[offset + 12] = static_cast<std::uint8_t>(member.playerName.size());
        std::copy(member.playerName.begin(), member.playerName.end(), payload.begin() + offset + 16);
        offset += 16 + member.playerName.size();
    }
    return true;
}

bool BuildPayload(const EndpointState& value, std::vector<std::uint8_t>& payload)
{
    if (value.playerId >= ProtocolMaximumPlayers ||
        value.generation == 0 || value.streamId == 0)
        return false;
    payload.assign(8, 0);
    payload[0] = value.playerId;
    payload[1] = value.active ? 1 : 0;
    Put16(payload, 2, value.generation);
    Put32(payload, 4, value.streamId);
    return true;
}

bool BuildPayload(const DSFrame& value, std::vector<std::uint8_t>& payload)
{
    if (!IsValidFrame(value))
        return false;
    payload.assign(28 + value.frame.size(), 0);
    payload[0] = value.senderPlayerId;
    payload[1] = value.destinationPlayerId;
    payload[2] = static_cast<std::uint8_t>(value.frameClass);
    payload[3] = value.associationId;
    Put16(payload, 4, value.senderGeneration);
    Put16(payload, 6, value.destinationGeneration);
    Put16(payload, 8, static_cast<std::uint16_t>(value.frame.size()));
    Put32(payload, 12, value.destinationStreamId);
    Put64(payload, 16, value.emulatedTimestamp);
    Put32(payload, 24, value.replyToCommandSequence);
    std::copy(value.frame.begin(), value.frame.end(), payload.begin() + 28);
    return true;
}

bool BuildPayload(const ChannelBarrier&, std::vector<std::uint8_t>& payload)
{
    payload.clear();
    return true;
}

MessageType PayloadType(const Payload& payload)
{
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ClientHello>)
            return MessageType::ClientHello;
        if constexpr (std::is_same_v<T, ServerWelcome>)
            return MessageType::ServerWelcome;
        if constexpr (std::is_same_v<T, JoinRejected>)
            return MessageType::JoinRejected;
        if constexpr (std::is_same_v<T, MembershipSnapshot>)
            return MessageType::MembershipSnapshot;
        if constexpr (std::is_same_v<T, EndpointState>)
            return MessageType::EndpointState;
        if constexpr (std::is_same_v<T, DSFrame>)
            return MessageType::DSFrame;
        return MessageType::ChannelBarrier;
    }, payload);
}

DecodeError ParseClientHello(const std::uint8_t* value, std::size_t size, Payload& result)
{
    if (size < 24 || Get16(value, 6) != 0)
        return size < 24 ? DecodeError::LengthMismatch : DecodeError::NonzeroReserved;
    const std::size_t nameLength = value[4];
    const std::size_t buildLength = value[5];
    if (size != 24 + nameLength + buildLength)
        return DecodeError::LengthMismatch;
    ClientHello hello;
    hello.coreNetworkEpoch = Get32(value, 0);
    hello.supportedFeatures = Get64(value, 8);
    hello.requiredFeatures = Get64(value, 16);
    hello.playerName.assign(reinterpret_cast<const char*>(value + 24), nameLength);
    hello.buildId.assign(reinterpret_cast<const char*>(value + 24 + nameLength), buildLength);
    std::vector<std::uint8_t> encoded;
    if (!BuildPayload(hello, encoded))
        return DecodeError::InvalidValue;
    result = std::move(hello);
    return DecodeError::None;
}

DecodeError ParseServerWelcome(const std::uint8_t* value, std::size_t size, Payload& result)
{
    if (size < 44)
        return DecodeError::LengthMismatch;
    if (value[8] != 0 || value[9] != 0 || Get16(value, 42) != 0)
        return DecodeError::NonzeroReserved;
    const std::size_t sessionNameLength = value[40];
    const std::size_t buildLength = value[41];
    if (size != 44 + sessionNameLength + buildLength)
        return DecodeError::LengthMismatch;
    ServerWelcome welcome;
    welcome.playerId = value[0];
    welcome.maxPlayers = value[1];
    welcome.playerGeneration = Get16(value, 2);
    welcome.playerStreamId = Get32(value, 4);
    welcome.hostGeneration = Get16(value, 10);
    welcome.hostStreamId = Get32(value, 12);
    welcome.membershipRevision = Get32(value, 16);
    welcome.coreNetworkEpoch = Get32(value, 20);
    welcome.supportedFeatures = Get64(value, 24);
    welcome.requiredFeatures = Get64(value, 32);
    welcome.sessionName.assign(reinterpret_cast<const char*>(value + 44), sessionNameLength);
    welcome.buildId.assign(reinterpret_cast<const char*>(value + 44 + sessionNameLength), buildLength);
    std::vector<std::uint8_t> encoded;
    if (!BuildPayload(welcome, encoded))
        return DecodeError::InvalidValue;
    result = std::move(welcome);
    return DecodeError::None;
}

DecodeError ParseJoinRejected(const std::uint8_t* value, std::size_t size, Payload& result)
{
    if (size < 20)
        return DecodeError::LengthMismatch;
    if (value[17] != 0 || value[18] != 0 || value[19] != 0)
        return DecodeError::NonzeroReserved;
    const std::size_t buildLength = value[16];
    if (size != 20 + buildLength)
        return DecodeError::LengthMismatch;
    JoinRejected rejection;
    rejection.reason = Get16(value, 0);
    rejection.serverProtocolMajor = value[2];
    rejection.serverProtocolMinor = value[3];
    rejection.serverCoreNetworkEpoch = Get32(value, 4);
    rejection.serverRequiredFeatures = Get64(value, 8);
    rejection.serverBuildId.assign(reinterpret_cast<const char*>(value + 20), buildLength);
    std::vector<std::uint8_t> encoded;
    if (!BuildPayload(rejection, encoded))
        return DecodeError::InvalidValue;
    result = std::move(rejection);
    return DecodeError::None;
}

DecodeError ParseMembership(const std::uint8_t* value, std::size_t size, Payload& result)
{
    if (size < 8 || Get16(value, 6) != 0)
        return size < 8 ? DecodeError::LengthMismatch : DecodeError::NonzeroReserved;
    if (value[4] > 1 || value[5] == 0 || value[5] > ProtocolMaximumPlayers)
        return DecodeError::InvalidValue;
    MembershipSnapshot snapshot;
    snapshot.revision = Get32(value, 0);
    snapshot.locked = value[4] != 0;
    std::size_t offset = 8;
    for (std::size_t index = 0; index < value[5]; ++index)
    {
        if (offset + 16 > size)
            return DecodeError::LengthMismatch;
        const std::size_t entryLength = Get16(value, offset);
        const std::size_t nameLength = value[offset + 12];
        if (entryLength != 16 + nameLength || offset + entryLength > size)
            return DecodeError::LengthMismatch;
        if (value[offset + 3] > 1 ||
            value[offset + 13] != 0 ||
            value[offset + 14] != 0 ||
            value[offset + 15] != 0)
            return DecodeError::NonzeroReserved;
        Member member;
        member.playerId = value[offset + 2];
        member.endpointActive = value[offset + 3] != 0;
        member.generation = Get16(value, offset + 4);
        member.pingMilliseconds = Get16(value, offset + 6);
        member.streamId = Get32(value, offset + 8);
        member.playerName.assign(
            reinterpret_cast<const char*>(value + offset + 16),
            nameLength);
        snapshot.members.push_back(std::move(member));
        offset += entryLength;
    }
    if (offset != size)
        return DecodeError::LengthMismatch;
    std::vector<std::uint8_t> encoded;
    if (!BuildPayload(snapshot, encoded))
        return DecodeError::InvalidValue;
    result = std::move(snapshot);
    return DecodeError::None;
}

DecodeError ParseFrame(const std::uint8_t* value, std::size_t size, Payload& result)
{
    if (size < 28)
        return DecodeError::LengthMismatch;
    if (Get16(value, 10) != 0)
        return DecodeError::NonzeroReserved;
    const std::size_t frameLength = Get16(value, 8);
    if (size != 28 + frameLength)
        return DecodeError::LengthMismatch;
    DSFrame frame;
    frame.senderPlayerId = value[0];
    frame.destinationPlayerId = value[1];
    frame.frameClass = static_cast<FrameClass>(value[2]);
    frame.associationId = value[3];
    frame.senderGeneration = Get16(value, 4);
    frame.destinationGeneration = Get16(value, 6);
    frame.destinationStreamId = Get32(value, 12);
    frame.emulatedTimestamp = Get64(value, 16);
    frame.replyToCommandSequence = Get32(value, 24);
    frame.frame.assign(value + 28, value + size);
    if (!IsValidFrame(frame))
        return DecodeError::InvalidFrame;
    result = std::move(frame);
    return DecodeError::None;
}

DecodeError ParseEndpointState(
    const std::uint8_t* value,
    std::size_t size,
    Payload& result)
{
    if (size != 8)
        return DecodeError::LengthMismatch;
    if (value[1] > 1)
        return DecodeError::InvalidValue;
    EndpointState state;
    state.playerId = value[0];
    state.active = value[1] != 0;
    state.generation = Get16(value, 2);
    state.streamId = Get32(value, 4);
    std::vector<std::uint8_t> encoded;
    if (!BuildPayload(state, encoded))
        return DecodeError::InvalidValue;
    result = state;
    return DecodeError::None;
}

}

bool Phase0ProtocolCodec::Encode(
    const Envelope& envelope,
    const Payload& payload,
    std::vector<std::uint8_t>& output)
{
    const MessageType type = PayloadType(payload);
    if (envelope.messageType != type)
        return false;

    const bool bootstrap =
        type == MessageType::ClientHello ||
        type == MessageType::ServerWelcome ||
        type == MessageType::JoinRejected;
    if (envelope.sequence == 0 ||
        (bootstrap && (envelope.sequence != 1 || envelope.streamId != 0)) ||
        (!bootstrap && envelope.sessionId == 0) ||
        (!bootstrap && envelope.streamId == 0))
        return false;

    std::vector<std::uint8_t> encodedPayload;
    const bool valid = std::visit([&encodedPayload](const auto& value) {
        return BuildPayload(value, encodedPayload);
    }, payload);
    if (!valid ||
        encodedPayload.size() > std::numeric_limits<std::uint16_t>::max() ||
        EnvelopeSize + encodedPayload.size() > MaximumApplicationPacketSize)
        return false;

    output.assign(EnvelopeSize + encodedPayload.size(), 0);
    std::copy(std::begin(Magic), std::end(Magic), output.begin());
    output[4] = FramingVersion;
    output[5] = envelope.protocolMajor;
    output[6] = envelope.protocolMinor;
    output[7] = static_cast<std::uint8_t>(type);
    Put16(output, 10, EnvelopeSize);
    Put16(output, 12, static_cast<std::uint16_t>(encodedPayload.size()));
    Put64(output, 16, envelope.sessionId);
    Put32(output, 24, envelope.sequence);
    Put32(output, 28, envelope.streamId);
    std::copy(encodedPayload.begin(), encodedPayload.end(), output.begin() + EnvelopeSize);
    return true;
}

DecodeResult Phase0ProtocolCodec::Decode(const std::uint8_t* bytes, std::size_t size)
{
    DecodeResult result;
    if (bytes == nullptr || size < EnvelopeSize)
    {
        result.error = DecodeError::PacketTooSmall;
        return result;
    }
    if (size > MaximumApplicationPacketSize)
    {
        result.error = DecodeError::PacketTooLarge;
        return result;
    }
    if (!std::equal(std::begin(Magic), std::end(Magic), bytes))
    {
        result.error = DecodeError::BadMagic;
        return result;
    }
    if (bytes[4] != FramingVersion)
    {
        result.error = DecodeError::UnsupportedFraming;
        return result;
    }
    if (!IsKnownMessage(bytes[7]))
    {
        result.error = DecodeError::UnknownMessageType;
        return result;
    }
    if (Get16(bytes, 8) != 0 || Get16(bytes, 14) != 0)
    {
        result.error = DecodeError::NonzeroReserved;
        return result;
    }
    if (Get16(bytes, 10) != EnvelopeSize)
    {
        result.error = DecodeError::InvalidHeaderLength;
        return result;
    }
    if (size != EnvelopeSize + Get16(bytes, 12))
    {
        result.error = DecodeError::LengthMismatch;
        return result;
    }

    result.packet.envelope.protocolMajor = bytes[5];
    result.packet.envelope.protocolMinor = bytes[6];
    result.packet.envelope.messageType = static_cast<MessageType>(bytes[7]);
    result.packet.envelope.sessionId = Get64(bytes, 16);
    result.packet.envelope.sequence = Get32(bytes, 24);
    result.packet.envelope.streamId = Get32(bytes, 28);

    const bool bootstrap =
        result.packet.envelope.messageType == MessageType::ClientHello ||
        result.packet.envelope.messageType == MessageType::ServerWelcome ||
        result.packet.envelope.messageType == MessageType::JoinRejected;
    if (result.packet.envelope.sequence == 0 ||
        (bootstrap &&
            (result.packet.envelope.sequence != 1 ||
             result.packet.envelope.streamId != 0)) ||
        (!bootstrap &&
            (result.packet.envelope.sessionId == 0 ||
             result.packet.envelope.streamId == 0)))
    {
        result.error = DecodeError::InvalidBootstrap;
        return result;
    }

    if (result.packet.envelope.protocolMajor != ProtocolMajor &&
        result.packet.envelope.messageType != MessageType::ClientHello &&
        result.packet.envelope.messageType != MessageType::JoinRejected)
    {
        result.error = DecodeError::UnsupportedProtocol;
        return result;
    }

    const auto* payload = bytes + EnvelopeSize;
    const std::size_t payloadSize = size - EnvelopeSize;
    switch (result.packet.envelope.messageType)
    {
    case MessageType::ClientHello:
        result.error = ParseClientHello(payload, payloadSize, result.packet.payload);
        break;
    case MessageType::ServerWelcome:
        result.error = ParseServerWelcome(payload, payloadSize, result.packet.payload);
        break;
    case MessageType::JoinRejected:
        result.error = ParseJoinRejected(payload, payloadSize, result.packet.payload);
        break;
    case MessageType::MembershipSnapshot:
        result.error = ParseMembership(payload, payloadSize, result.packet.payload);
        break;
    case MessageType::EndpointState:
        result.error = ParseEndpointState(payload, payloadSize, result.packet.payload);
        break;
    case MessageType::DSFrame:
        result.error = ParseFrame(payload, payloadSize, result.packet.payload);
        break;
    case MessageType::ChannelBarrier:
        if (payloadSize != 0)
            result.error = DecodeError::LengthMismatch;
        else
            result.packet.payload = ChannelBarrier{};
        break;
    }
    return result;
}

bool Phase0ProtocolCodec::IsSequenceNewer(
    std::uint32_t candidate,
    std::uint32_t last) noexcept
{
    return static_cast<std::int32_t>(candidate - last) > 0;
}

std::uint32_t Phase0ProtocolCodec::NextSequence(std::uint32_t current) noexcept
{
    ++current;
    return current == 0 ? 1 : current;
}

}
