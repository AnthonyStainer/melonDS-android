#ifndef MELONDS_ANDROID_PHASE0_LAN_CORE_ADAPTER_H
#define MELONDS_ANDROID_PHASE0_LAN_CORE_ADAPTER_H

#include "Phase0ProtocolCodec.h"

#include <MPInterface.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace MelonDSAndroid::Multiplayer
{

struct MemberIdentity
{
    std::uint8_t playerId = 0;
    std::uint16_t generation = 0;
    std::uint32_t streamId = 0;

    bool operator==(const MemberIdentity& other) const noexcept
    {
        return playerId == other.playerId &&
            generation == other.generation &&
            streamId == other.streamId;
    }

    bool operator<(const MemberIdentity& other) const noexcept
    {
        if (playerId != other.playerId)
            return playerId < other.playerId;
        if (generation != other.generation)
            return generation < other.generation;
        return streamId < other.streamId;
    }

    explicit operator bool() const noexcept
    {
        return generation != 0 && streamId != 0;
    }
};

struct CoreFrameValue
{
    MemberIdentity origin;
    std::optional<MemberIdentity> destination;
    FrameClass frameClass = FrameClass::Regular;
    std::uint8_t associationId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t replyToCommandSequence = 0;
    std::uint64_t emulatedTimestamp = 0;
    std::vector<std::uint8_t> bytes;
};

enum class SequenceClass : std::size_t
{
    Control = 0,
    Regular = 1,
    Command = 2,
    Reply = 3,
    Ack = 4,
    Barrier = 5,
};

class Phase0LanCoreAdapter final : public melonDS::MPInterface
{
public:
    using FrameSink = std::function<bool(CoreFrameValue)>;
    using EndpointSink = std::function<void(bool)>;
    using AuthoritativeExhaustion =
        std::function<bool(const MemberIdentity&, std::uint32_t)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    Phase0LanCoreAdapter(
        MemberIdentity localIdentity,
        bool applicationHostLocal,
        FrameSink frameSink,
        EndpointSink endpointSink = {},
        AuthoritativeExhaustion authoritativeExhaustion = {},
        Clock clock = std::chrono::steady_clock::now);
    ~Phase0LanCoreAdapter() override;

    void Process() override {}
    void Begin(int instance) override;
    void End(int instance) override;

    int SendPacket(
        int instance,
        melonDS::u8* data,
        int length,
        melonDS::u64 timestamp) override;
    int RecvPacket(
        int instance,
        melonDS::u8* data,
        melonDS::u64* timestamp) override;
    int SendCmd(
        int instance,
        melonDS::u8* data,
        int length,
        melonDS::u64 timestamp) override;
    int SendReply(
        int instance,
        melonDS::u8* data,
        int length,
        melonDS::u64 timestamp,
        melonDS::u16 associationId) override;
    int SendAck(
        int instance,
        melonDS::u8* data,
        int length,
        melonDS::u64 timestamp) override;
    int RecvHostPacket(
        int instance,
        melonDS::u8* data,
        melonDS::u64* timestamp) override;
    melonDS::u16 RecvReplies(
        int instance,
        melonDS::u8* data,
        melonDS::u64 timestamp,
        melonDS::u16 associationIdMask) override;

    bool Deliver(CoreFrameValue frame);
    void MemberUnavailable(const MemberIdentity& identity);
    void StopSession();
    bool IsEndpointActive() const;
    std::uint32_t NextSequence(SequenceClass sequenceClass);

private:
    static constexpr std::size_t MaximumIncomingFrames = 256;
    static constexpr std::size_t MaximumIncomingBytes = 512 * 1024;
    static constexpr std::size_t ReplySlotSize = 1024;
    static constexpr std::size_t ReplySlotCount = 15;
    static constexpr std::size_t MaximumInstances = 16;

    struct InboundCommandContext
    {
        MemberIdentity origin;
        std::uint32_t commandSequence = 0;
        std::uint64_t emulatedTimestamp = 0;
        std::chrono::steady_clock::time_point expiresAt{};
    };

    struct ReplyValue
    {
        MemberIdentity origin;
        std::uint8_t associationId = 0;
        std::vector<std::uint8_t> bytes;
    };

    struct OutboundCommandContext
    {
        std::uint32_t commandSequence = 0;
        std::uint64_t emulatedTimestamp = 0;
        std::chrono::steady_clock::time_point expiresAt{};
        std::array<std::optional<ReplyValue>, ReplySlotCount> replies;
        std::set<MemberIdentity> responders;
    };

    static bool ValidateCoreFrame(
        FrameClass frameClass,
        const melonDS::u8* data,
        int length,
        melonDS::u16 associationId);
    static std::uint16_t InternalFrameLength(const melonDS::u8* data);

    int SendBroadcast(
        FrameClass frameClass,
        melonDS::u8* data,
        int length,
        melonDS::u64 timestamp,
        std::uint32_t correlation);
    int Receive(
        int instance,
        melonDS::u8* data,
        melonDS::u64* timestamp,
        bool block);
    std::optional<std::size_t> FindDeliverableFrameLocked(int instance);
    std::chrono::milliseconds ExchangeLifetimeLocked() const;
    bool InstanceValid(int instance) const noexcept;

    MemberIdentity LocalIdentity;
    bool ApplicationHostLocal;
    FrameSink Sink;
    EndpointSink EndpointChanged;
    AuthoritativeExhaustion IsAuthoritativelyExhausted;
    Clock Now;

    mutable std::mutex Mutex;
    std::condition_variable Condition;
    bool Stopped = false;
    std::uint16_t ActiveInstances = 0;
    std::deque<CoreFrameValue> IncomingFrames;
    std::size_t IncomingBytes = 0;
    std::array<std::optional<InboundCommandContext>, MaximumInstances> InboundContexts;
    std::array<std::optional<OutboundCommandContext>, MaximumInstances> OutboundContexts;
    std::optional<MemberIdentity> LogicalLastHost;
    bool LogicalLastHostAvailable = false;
    std::array<std::uint32_t, 6> Sequences{};
};

}

#endif
