#ifndef MELONDS_ANDROID_PHASE0_PROTOCOL_CODEC_H
#define MELONDS_ANDROID_PHASE0_PROTOCOL_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace MelonDSAndroid::Multiplayer
{

constexpr std::size_t EnvelopeSize = 32;
constexpr std::size_t MaximumApplicationPacketSize = 4096;
constexpr std::uint8_t FramingVersion = 1;
constexpr std::uint8_t ProtocolMajor = 2;
constexpr std::uint8_t ProtocolMinor = 0;
constexpr std::uint32_t CoreNetworkEpoch = 1;
constexpr std::uint64_t BaselineFeatures = 0x0F;
constexpr std::uint8_t ProtocolMaximumPlayers = 16;
constexpr std::uint8_t InitialQualifiedMaxPlayers = 4;

enum class MessageType : std::uint8_t
{
    ClientHello = 0x01,
    ServerWelcome = 0x02,
    JoinRejected = 0x03,
    MembershipSnapshot = 0x04,
    EndpointState = 0x05,
    DSFrame = 0x20,
    ChannelBarrier = 0x21,
};

enum class FrameClass : std::uint8_t
{
    Regular = 0,
    Command = 1,
    Reply = 2,
    Ack = 3,
};

enum class DecodeError
{
    None,
    PacketTooSmall,
    PacketTooLarge,
    BadMagic,
    UnsupportedFraming,
    UnsupportedProtocol,
    UnknownMessageType,
    NonzeroReserved,
    InvalidHeaderLength,
    LengthMismatch,
    InvalidBootstrap,
    InvalidValue,
    InvalidUtf8,
    InvalidFrame,
};

struct Envelope
{
    std::uint8_t protocolMajor = ProtocolMajor;
    std::uint8_t protocolMinor = ProtocolMinor;
    MessageType messageType = MessageType::ClientHello;
    std::uint64_t sessionId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t streamId = 0;
};

struct ClientHello
{
    std::uint32_t coreNetworkEpoch = CoreNetworkEpoch;
    std::uint64_t supportedFeatures = BaselineFeatures;
    std::uint64_t requiredFeatures = BaselineFeatures;
    std::string playerName;
    std::string buildId;
};

struct ServerWelcome
{
    std::uint8_t playerId = 0;
    std::uint8_t maxPlayers = InitialQualifiedMaxPlayers;
    std::uint16_t playerGeneration = 0;
    std::uint32_t playerStreamId = 0;
    std::uint16_t hostGeneration = 1;
    std::uint32_t hostStreamId = 0;
    std::uint32_t membershipRevision = 0;
    std::uint32_t coreNetworkEpoch = CoreNetworkEpoch;
    std::uint64_t supportedFeatures = BaselineFeatures;
    std::uint64_t requiredFeatures = BaselineFeatures;
    std::string sessionName;
    std::string buildId;
};

struct JoinRejected
{
    std::uint16_t reason = 0;
    std::uint8_t serverProtocolMajor = ProtocolMajor;
    std::uint8_t serverProtocolMinor = ProtocolMinor;
    std::uint32_t serverCoreNetworkEpoch = CoreNetworkEpoch;
    std::uint64_t serverRequiredFeatures = BaselineFeatures;
    std::string serverBuildId;
};

struct Member
{
    std::uint8_t playerId = 0;
    bool endpointActive = false;
    std::uint16_t generation = 0;
    std::uint16_t pingMilliseconds = 0xFFFF;
    std::uint32_t streamId = 0;
    std::string playerName;
};

struct MembershipSnapshot
{
    std::uint32_t revision = 0;
    bool locked = false;
    std::vector<Member> members;
};

struct EndpointState
{
    std::uint8_t playerId = 0;
    bool active = false;
    std::uint16_t generation = 0;
    std::uint32_t streamId = 0;
};

struct DSFrame
{
    std::uint8_t senderPlayerId = 0;
    std::uint8_t destinationPlayerId = 0xFF;
    FrameClass frameClass = FrameClass::Regular;
    std::uint8_t associationId = 0;
    std::uint16_t senderGeneration = 0;
    std::uint16_t destinationGeneration = 0;
    std::uint32_t destinationStreamId = 0;
    std::uint64_t emulatedTimestamp = 0;
    std::uint32_t replyToCommandSequence = 0;
    std::vector<std::uint8_t> frame;
};

struct ChannelBarrier
{
};

using Payload = std::variant<
    ClientHello,
    ServerWelcome,
    JoinRejected,
    MembershipSnapshot,
    EndpointState,
    DSFrame,
    ChannelBarrier>;

struct DecodedPacket
{
    Envelope envelope;
    Payload payload;
};

struct DecodeResult
{
    DecodeError error = DecodeError::None;
    DecodedPacket packet;

    explicit operator bool() const noexcept { return error == DecodeError::None; }
};

class Phase0ProtocolCodec
{
public:
    static bool Encode(
        const Envelope& envelope,
        const Payload& payload,
        std::vector<std::uint8_t>& output);

    static DecodeResult Decode(const std::uint8_t* bytes, std::size_t size);

    static bool IsSequenceNewer(std::uint32_t candidate, std::uint32_t last) noexcept;
    static std::uint32_t NextSequence(std::uint32_t current) noexcept;
};

}

#endif
