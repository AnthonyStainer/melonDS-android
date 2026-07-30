#include "multiplayer/Phase0EnetWorker.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>

using namespace MelonDSAndroid::Multiplayer;
using namespace std::chrono_literals;

namespace
{

struct Events
{
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<WorkerEvent> values;

    void Add(WorkerEvent event)
    {
        {
            std::lock_guard lock(mutex);
            values.push_back(std::move(event));
        }
        condition.notify_all();
    }

    bool WaitFor(WorkerEventType type, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [&] {
            for (const WorkerEvent& event : values)
            {
                if (event.type == type)
                    return true;
            }
            return false;
        });
    }

    bool WaitForError(WorkerError error, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [&] {
            for (const WorkerEvent& event : values)
            {
                if (event.error == error)
                    return true;
            }
            return false;
        });
    }
};

std::vector<std::uint8_t> Barrier()
{
    Envelope envelope;
    envelope.messageType = MessageType::ChannelBarrier;
    envelope.sessionId = 1;
    envelope.sequence = 1;
    envelope.streamId = 1;
    std::vector<std::uint8_t> packet;
    assert(Phase0ProtocolCodec::Encode(envelope, ChannelBarrier{}, packet));
    return packet;
}

void TestPortCollisionAndSaturatedShutdown()
{
    constexpr std::uint16_t Port = 57064;
    Events hostEvents;
    WorkerSettings hostSettings;
    hostSettings.role = WorkerRole::Host;
    hostSettings.port = Port;
    Phase0EnetWorker host(hostSettings, [&](WorkerEvent event) {
        if (event.type == WorkerEventType::Connected)
            host.MarkWelcomed(event.peerIndex);
        hostEvents.Add(std::move(event));
    });
    assert(host.Start());
    assert(hostEvents.WaitFor(WorkerEventType::Started, 2s));

    Events collisionEvents;
    Phase0EnetWorker collision(hostSettings, [&](WorkerEvent event) {
        collisionEvents.Add(std::move(event));
    });
    assert(collision.Start());
    assert(collisionEvents.WaitForError(WorkerError::PortUnavailable, 2s));
    assert(collision.WaitForStopped(2s));
    collision.Join();

    Events clientEvents;
    WorkerSettings clientSettings;
    clientSettings.role = WorkerRole::Client;
    clientSettings.port = Port;
    clientSettings.remoteIpv4 = {127, 0, 0, 1};
    Phase0EnetWorker client(clientSettings, [&](WorkerEvent event) {
        clientEvents.Add(std::move(event));
    });
    assert(client.Start());
    assert(clientEvents.WaitFor(WorkerEventType::Connected, 2s));
    assert(hostEvents.WaitFor(WorkerEventType::Connected, 2s));

    const auto barrier = Barrier();
    std::size_t accepted = 0;
    while (client.Enqueue(0, 1, ENET_PACKET_FLAG_RELIABLE, barrier))
        ++accepted;
    assert(accepted > 0);

    const auto started = std::chrono::steady_clock::now();
    client.Stop();
    host.Stop();
    assert(client.WaitForStopped(ShutdownSlo));
    assert(host.WaitForStopped(ShutdownSlo));
    client.Join();
    host.Join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    assert(elapsed < ShutdownSlo);
    assert(client.ReliableBytesForHost() == 0);
    assert(host.ReliableBytesForHost() == 0);
}

}

int main()
{
    TestPortCollisionAndSaturatedShutdown();
    std::cout << "Phase0EnetWorkerTest passed\n";
    return 0;
}
