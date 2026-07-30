#include "Phase0LanCoreAdapter.h"
#include "Phase0TraceRecorder.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace MelonDSAndroid::Multiplayer
{

Phase0LanCoreAdapter::Phase0LanCoreAdapter(
    MemberIdentity localIdentity,
    bool applicationHostLocal,
    FrameSink frameSink,
    EndpointSink endpointSink,
    AuthoritativeExhaustion authoritativeExhaustion,
    Clock clock)
    : LocalIdentity(localIdentity),
      ApplicationHostLocal(applicationHostLocal),
      Sink(std::move(frameSink)),
      EndpointChanged(std::move(endpointSink)),
      IsAuthoritativelyExhausted(std::move(authoritativeExhaustion)),
      Now(std::move(clock))
{
    if (!Now)
        Now = std::chrono::steady_clock::now;
}

Phase0LanCoreAdapter::~Phase0LanCoreAdapter()
{
    StopSession();
}

void Phase0LanCoreAdapter::Begin(int instance)
{
    if (!InstanceValid(instance))
        return;
    bool becameActive = false;
    {
        std::lock_guard lock(Mutex);
        const std::uint16_t prior = ActiveInstances;
        ActiveInstances |= static_cast<std::uint16_t>(1u << instance);
        becameActive = prior == 0 && ActiveInstances != 0;
    }
    if (becameActive && EndpointChanged)
        EndpointChanged(true);
}

void Phase0LanCoreAdapter::End(int instance)
{
    if (!InstanceValid(instance))
        return;
    bool becameInactive = false;
    {
        std::lock_guard lock(Mutex);
        ActiveInstances &= static_cast<std::uint16_t>(~(1u << instance));
        InboundContexts[instance].reset();
        OutboundContexts[instance].reset();
        becameInactive = ActiveInstances == 0;
    }
    Condition.notify_all();
    if (becameInactive && EndpointChanged)
        EndpointChanged(false);
}

int Phase0LanCoreAdapter::SendPacket(
    int,
    melonDS::u8* data,
    int length,
    melonDS::u64 timestamp)
{
    return SendBroadcast(FrameClass::Regular, data, length, timestamp, 0);
}

int Phase0LanCoreAdapter::RecvPacket(
    int instance,
    melonDS::u8* data,
    melonDS::u64* timestamp)
{
    return Receive(instance, data, timestamp, false);
}

int Phase0LanCoreAdapter::SendCmd(
    int instance,
    melonDS::u8* data,
    int length,
    melonDS::u64 timestamp)
{
    if (!InstanceValid(instance) ||
        !ValidateCoreFrame(FrameClass::Command, data, length, 0))
        return 0;

    CoreFrameValue frame;
    frame.origin = LocalIdentity;
    frame.frameClass = FrameClass::Command;
    frame.sequence = NextSequence(SequenceClass::Command);
    frame.emulatedTimestamp = timestamp;
    frame.bytes.assign(data, data + length);
    Phase0TraceRecorder::Get().Record(
        TraceEventType::SendCommandEntry,
        frame.sequence,
        timestamp,
        static_cast<std::uint32_t>(length),
        LocalIdentity.playerId);
    {
        std::lock_guard lock(Mutex);
        if (Stopped || (ActiveInstances & (1u << instance)) == 0)
            return 0;
    }
    if (!Sink || !Sink(frame))
        return 0;

    {
        std::lock_guard lock(Mutex);
        OutboundCommandContext context;
        context.commandSequence = frame.sequence;
        context.emulatedTimestamp = timestamp;
        context.expiresAt = Now() + ExchangeLifetimeLocked();
        OutboundContexts[instance] = std::move(context);
    }
    Condition.notify_all();
    return length;
}

int Phase0LanCoreAdapter::SendReply(
    int instance,
    melonDS::u8* data,
    int length,
    melonDS::u64 timestamp,
    melonDS::u16 associationId)
{
    if (!InstanceValid(instance) ||
        !ValidateCoreFrame(FrameClass::Reply, data, length, associationId))
        return 0;

    InboundCommandContext context;
    {
        std::lock_guard lock(Mutex);
        if (Stopped || !InboundContexts[instance] ||
            Now() >= InboundContexts[instance]->expiresAt)
        {
            InboundContexts[instance].reset();
            return 0;
        }
        context = *InboundContexts[instance];
    }

    CoreFrameValue frame;
    frame.origin = LocalIdentity;
    frame.destination = context.origin;
    frame.frameClass = FrameClass::Reply;
    frame.associationId = static_cast<std::uint8_t>(associationId);
    frame.sequence = NextSequence(SequenceClass::Reply);
    frame.replyToCommandSequence = context.commandSequence;
    frame.emulatedTimestamp = timestamp;
    Phase0TraceRecorder::Get().Record(
        TraceEventType::SendReplyEntry,
        context.commandSequence,
        timestamp,
        static_cast<std::uint32_t>(length),
        LocalIdentity.playerId);
    if (length > 0)
        frame.bytes.assign(data, data + length);
    if (!Sink || !Sink(std::move(frame)))
        return 0;

    {
        std::lock_guard lock(Mutex);
        if (InboundContexts[instance] &&
            InboundContexts[instance]->commandSequence == context.commandSequence &&
            InboundContexts[instance]->origin == context.origin)
            InboundContexts[instance].reset();
    }
    Condition.notify_all();
    return length;
}

int Phase0LanCoreAdapter::SendAck(
    int instance,
    melonDS::u8* data,
    int length,
    melonDS::u64 timestamp)
{
    if (!InstanceValid(instance) ||
        !ValidateCoreFrame(FrameClass::Ack, data, length, 0))
        return 0;
    std::uint32_t correlation = 0;
    {
        std::lock_guard lock(Mutex);
        if (Stopped || !OutboundContexts[instance])
            return 0;
        correlation = OutboundContexts[instance]->commandSequence;
    }
    const int result = SendBroadcast(
        FrameClass::Ack,
        data,
        length,
        timestamp,
        correlation);
    if (result != 0)
    {
        std::lock_guard lock(Mutex);
        if (OutboundContexts[instance] &&
            OutboundContexts[instance]->commandSequence == correlation)
            OutboundContexts[instance].reset();
    }
    return result;
}

int Phase0LanCoreAdapter::RecvHostPacket(
    int instance,
    melonDS::u8* data,
    melonDS::u64* timestamp)
{
    {
        std::lock_guard lock(Mutex);
        if (Stopped ||
            (LogicalLastHost && !LogicalLastHostAvailable))
            return -1;
    }
    return Receive(instance, data, timestamp, true);
}

melonDS::u16 Phase0LanCoreAdapter::RecvReplies(
    int instance,
    melonDS::u8* data,
    melonDS::u64,
    melonDS::u16 associationIdMask)
{
    if (data != nullptr)
        std::memset(data, 0, ReplySlotCount * ReplySlotSize);
    if (!InstanceValid(instance) || data == nullptr)
        return 0;

    std::unique_lock lock(Mutex);
    auto deadline = Now() + std::chrono::milliseconds(GetRecvTimeout());
    melonDS::u16 receivedMask = 0;
    const auto finish = [&](melonDS::u16 mask) {
        Phase0TraceRecorder::Get().Record(
            TraceEventType::ReceiveRepliesReturn,
            OutboundContexts[instance]
                ? OutboundContexts[instance]->commandSequence
                : 0,
            0,
            mask,
            LocalIdentity.playerId);
        return mask;
    };
    for (;;)
    {
        if (Stopped || !OutboundContexts[instance])
            return finish(0);
        OutboundCommandContext& context = *OutboundContexts[instance];
        receivedMask = 0;
        for (const auto& reply : context.replies)
        {
            if (!reply)
                continue;
            receivedMask |= static_cast<melonDS::u16>(1u << reply->associationId);
            std::memcpy(
                data + (reply->associationId - 1) * ReplySlotSize,
                reply->bytes.data(),
                reply->bytes.size());
        }
        if ((receivedMask & associationIdMask) == associationIdMask)
            return finish(receivedMask);
        if (ApplicationHostLocal && IsAuthoritativelyExhausted &&
            IsAuthoritativelyExhausted(LocalIdentity, context.commandSequence))
            return finish(receivedMask);

        const auto now = Now();
        if (now >= deadline || now >= context.expiresAt)
            return finish(receivedMask);
        Condition.wait_until(lock, std::min(deadline, context.expiresAt));
    }
}

bool Phase0LanCoreAdapter::Deliver(CoreFrameValue frame)
{
    if (!frame.origin || frame.origin == LocalIdentity ||
        frame.sequence == 0 ||
        !ValidateCoreFrame(
            frame.frameClass,
            frame.bytes.empty() ? nullptr : frame.bytes.data(),
            static_cast<int>(frame.bytes.size()),
            frame.associationId))
        return false;

    std::lock_guard lock(Mutex);
    if (Stopped)
        return false;
    Phase0TraceRecorder::Get().Record(
        TraceEventType::AdapterIngress,
        frame.sequence,
        frame.emulatedTimestamp,
        static_cast<std::uint32_t>(frame.bytes.size()),
        frame.origin.playerId);

    if (frame.frameClass == FrameClass::Reply)
    {
        if (!frame.destination || !(*frame.destination == LocalIdentity) ||
            frame.replyToCommandSequence == 0)
            return false;
        bool accepted = false;
        for (auto& context : OutboundContexts)
        {
            if (!context ||
                context->commandSequence != frame.replyToCommandSequence ||
                Now() >= context->expiresAt ||
                context->responders.find(frame.origin) != context->responders.end())
                continue;
            context->responders.insert(frame.origin);
            if (!frame.bytes.empty())
            {
                auto& slot = context->replies[frame.associationId - 1];
                if (!slot)
                {
                    slot = ReplyValue{
                        frame.origin,
                        frame.associationId,
                        std::move(frame.bytes),
                    };
                }
            }
            accepted = true;
            break;
        }
        if (accepted)
            Condition.notify_all();
        return accepted;
    }

    if (IncomingFrames.size() >= MaximumIncomingFrames ||
        IncomingBytes + frame.bytes.size() > MaximumIncomingBytes)
        return false;
    IncomingBytes += frame.bytes.size();
    IncomingFrames.push_back(std::move(frame));
    Condition.notify_all();
    return true;
}

void Phase0LanCoreAdapter::MemberUnavailable(const MemberIdentity& identity)
{
    {
        std::lock_guard lock(Mutex);
        if (LogicalLastHost && *LogicalLastHost == identity)
            LogicalLastHostAvailable = false;
        for (auto& context : InboundContexts)
        {
            if (context && context->origin == identity)
                context.reset();
        }
    }
    Condition.notify_all();
}

void Phase0LanCoreAdapter::StopSession()
{
    {
        std::lock_guard lock(Mutex);
        if (Stopped)
            return;
        Stopped = true;
        IncomingFrames.clear();
        IncomingBytes = 0;
        for (auto& context : InboundContexts)
            context.reset();
        for (auto& context : OutboundContexts)
            context.reset();
        LogicalLastHostAvailable = false;
    }
    Condition.notify_all();
}

bool Phase0LanCoreAdapter::IsEndpointActive() const
{
    std::lock_guard lock(Mutex);
    return ActiveInstances != 0;
}

std::uint32_t Phase0LanCoreAdapter::NextSequence(SequenceClass sequenceClass)
{
    std::lock_guard lock(Mutex);
    auto& sequence = Sequences[static_cast<std::size_t>(sequenceClass)];
    sequence = Phase0ProtocolCodec::NextSequence(sequence);
    return sequence;
}

bool Phase0LanCoreAdapter::ValidateCoreFrame(
    FrameClass frameClass,
    const melonDS::u8* data,
    int length,
    melonDS::u16 associationId)
{
    if (frameClass == FrameClass::Reply && length == 0)
        return associationId == 0;
    if (data == nullptr)
        return false;
    if (frameClass == FrameClass::Reply)
    {
        if (associationId == 0 || associationId > 15 ||
            length < 36 || length > 1024)
            return false;
    }
    else
    {
        if (associationId != 0 || length < 36 || length > 0x948)
            return false;
    }
    return InternalFrameLength(data) == length - 12;
}

std::uint16_t Phase0LanCoreAdapter::InternalFrameLength(const melonDS::u8* data)
{
    return static_cast<std::uint16_t>(
        data[10] | (static_cast<std::uint16_t>(data[11]) << 8));
}

int Phase0LanCoreAdapter::SendBroadcast(
    FrameClass frameClass,
    melonDS::u8* data,
    int length,
    melonDS::u64 timestamp,
    std::uint32_t correlation)
{
    if (!ValidateCoreFrame(frameClass, data, length, 0))
        return 0;
    {
        std::lock_guard lock(Mutex);
        if (Stopped || ActiveInstances == 0)
            return 0;
    }
    SequenceClass sequenceClass = SequenceClass::Regular;
    if (frameClass == FrameClass::Ack)
        sequenceClass = SequenceClass::Ack;
    CoreFrameValue frame;
    frame.origin = LocalIdentity;
    frame.frameClass = frameClass;
    frame.sequence = NextSequence(sequenceClass);
    frame.replyToCommandSequence = correlation;
    frame.emulatedTimestamp = timestamp;
    frame.bytes.assign(data, data + length);
    return Sink && Sink(std::move(frame)) ? length : 0;
}

int Phase0LanCoreAdapter::Receive(
    int instance,
    melonDS::u8* data,
    melonDS::u64* timestamp,
    bool block)
{
    if (!InstanceValid(instance) || data == nullptr)
        return 0;
    std::unique_lock lock(Mutex);
    const auto deadline = Now() + std::chrono::milliseconds(GetRecvTimeout());
    for (;;)
    {
        if (Stopped)
            return block ? -1 : 0;
        const auto frameIndex = FindDeliverableFrameLocked(instance);
        if (frameIndex)
        {
            CoreFrameValue frame = std::move(IncomingFrames[*frameIndex]);
            IncomingBytes -= frame.bytes.size();
            IncomingFrames.erase(IncomingFrames.begin() + *frameIndex);
            if (frame.frameClass == FrameClass::Command)
            {
                InboundContexts[instance] = InboundCommandContext{
                    frame.origin,
                    frame.sequence,
                    frame.emulatedTimestamp,
                    Now() + ExchangeLifetimeLocked(),
                };
                LogicalLastHost = frame.origin;
                LogicalLastHostAvailable = true;
            }
            std::memcpy(data, frame.bytes.data(), frame.bytes.size());
            if (timestamp != nullptr)
                *timestamp = frame.emulatedTimestamp;
            if (block)
            {
                Phase0TraceRecorder::Get().Record(
                    TraceEventType::ReceiveHostReturn,
                    frame.sequence,
                    frame.emulatedTimestamp,
                    static_cast<std::uint32_t>(frame.bytes.size()),
                    LocalIdentity.playerId);
            }
            return static_cast<int>(frame.bytes.size());
        }
        if (!block)
            return 0;
        if (LogicalLastHost && !LogicalLastHostAvailable)
            return -1;
        if (Now() >= deadline)
            return 0;
        Condition.wait_until(lock, deadline);
    }
}

std::optional<std::size_t>
Phase0LanCoreAdapter::FindDeliverableFrameLocked(int instance)
{
    if (InboundContexts[instance] && Now() >= InboundContexts[instance]->expiresAt)
        InboundContexts[instance].reset();
    for (std::size_t index = 0; index < IncomingFrames.size(); ++index)
    {
        if (IncomingFrames[index].frameClass != FrameClass::Command ||
            !InboundContexts[instance])
            return index;
    }
    return std::nullopt;
}

std::chrono::milliseconds Phase0LanCoreAdapter::ExchangeLifetimeLocked() const
{
    return std::min(
        std::chrono::milliseconds(500),
        std::max(
            std::chrono::milliseconds(100),
            std::chrono::milliseconds(4 * GetRecvTimeout())));
}

bool Phase0LanCoreAdapter::InstanceValid(int instance) const noexcept
{
    return instance >= 0 && instance < static_cast<int>(MaximumInstances);
}

}
