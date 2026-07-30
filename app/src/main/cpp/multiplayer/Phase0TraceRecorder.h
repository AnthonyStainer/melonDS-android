#ifndef MELONDS_ANDROID_PHASE0_TRACE_RECORDER_H
#define MELONDS_ANDROID_PHASE0_TRACE_RECORDER_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace MelonDSAndroid::Multiplayer
{

enum class TraceEventType : std::uint16_t
{
    SendCommandEntry = 1,
    CoreToWorkerEnqueue = 2,
    WorkerCommandDequeue = 3,
    EnetService = 4,
    TransportIngress = 5,
    RouterAccepted = 6,
    RelayEnqueue = 7,
    AdapterIngress = 8,
    ReceiveHostReturn = 9,
    SendReplyEntry = 10,
    ReceiveRepliesReturn = 11,
    DeadlineMiss = 12,
};

struct TraceEvent
{
    std::uint64_t monotonicNanoseconds = 0;
    std::uint64_t emulatedTimestamp = 0;
    std::uint32_t sequence = 0;
    std::uint32_t value = 0;
    std::uint16_t type = 0;
    std::uint8_t playerId = 0;
    std::uint8_t peerIndex = 0xFF;
};

class Phase0TraceRecorder
{
public:
    static Phase0TraceRecorder& Get();

    void Record(
        TraceEventType type,
        std::uint32_t sequence = 0,
        std::uint64_t emulatedTimestamp = 0,
        std::uint32_t value = 0,
        std::uint8_t playerId = 0,
        std::uint8_t peerIndex = 0xFF);
    std::vector<TraceEvent> Drain();
    std::uint64_t Dropped() const;

private:
    static constexpr std::size_t MaximumEvents = 65536;
    mutable std::mutex Mutex;
    std::vector<TraceEvent> Events;
    std::uint64_t DroppedEvents = 0;
};

}

#endif
