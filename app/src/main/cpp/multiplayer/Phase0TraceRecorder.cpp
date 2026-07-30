#include "Phase0TraceRecorder.h"

#include <chrono>

namespace MelonDSAndroid::Multiplayer
{

Phase0TraceRecorder& Phase0TraceRecorder::Get()
{
    static Phase0TraceRecorder recorder;
    return recorder;
}

void Phase0TraceRecorder::Record(
    TraceEventType type,
    std::uint32_t sequence,
    std::uint64_t emulatedTimestamp,
    std::uint32_t value,
    std::uint8_t playerId,
    std::uint8_t peerIndex)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    TraceEvent event;
    event.monotonicNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    event.emulatedTimestamp = emulatedTimestamp;
    event.sequence = sequence;
    event.value = value;
    event.type = static_cast<std::uint16_t>(type);
    event.playerId = playerId;
    event.peerIndex = peerIndex;
    std::lock_guard lock(Mutex);
    if (Events.size() >= MaximumEvents)
    {
        ++DroppedEvents;
        return;
    }
    Events.push_back(event);
}

std::vector<TraceEvent> Phase0TraceRecorder::Drain()
{
    std::lock_guard lock(Mutex);
    std::vector<TraceEvent> events;
    events.swap(Events);
    return events;
}

std::uint64_t Phase0TraceRecorder::Dropped() const
{
    std::lock_guard lock(Mutex);
    return DroppedEvents;
}

}
