#include "Phase0EnetWorker.h"
#include "Phase0ProtocolCodec.h"

#include <jni.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace MelonDSAndroid::Multiplayer
{
namespace
{

constexpr std::size_t MaximumPendingEvents = 256;
constexpr std::size_t EventHeaderSize = 12;

class TransportHandle
{
public:
    explicit TransportHandle(WorkerSettings settings)
        : Worker(std::move(settings), [this](WorkerEvent event) {
            OnEvent(std::move(event));
        })
    {
    }

    bool Start() { return Worker.Start(); }

    bool StopAndWait(std::chrono::milliseconds timeout)
    {
        Worker.Stop();
        if (!Worker.WaitForStopped(timeout))
            return false;
        Worker.Join();
        return true;
    }

    bool AwaitStopped(std::chrono::milliseconds timeout)
    {
        if (!Worker.WaitForStopped(timeout))
            return false;
        Worker.Join();
        return true;
    }

    bool Send(
        std::size_t peerIndex,
        std::uint8_t channel,
        std::uint32_t flags,
        std::vector<std::uint8_t> packet)
    {
        return Worker.Enqueue(peerIndex, channel, flags, std::move(packet));
    }

    bool MarkWelcomed(std::size_t peerIndex)
    {
        return Worker.MarkWelcomed(peerIndex);
    }

    std::optional<WorkerEvent> Poll(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(Mutex);
        if (!Condition.wait_for(lock, timeout, [this] { return !Events.empty(); }))
            return std::nullopt;
        WorkerEvent event = std::move(Events.front());
        Events.pop_front();
        return event;
    }

    bool IsRunning() const { return Worker.IsRunning(); }

private:
    void OnEvent(WorkerEvent event)
    {
        {
            std::lock_guard lock(Mutex);
            if (Events.size() >= MaximumPendingEvents)
            {
                // The polling probe is no longer observing lifecycle state.
                // Stop through the out-of-band atomic path; never evict a
                // critical transport event to make room.
                Worker.Stop();
                return;
            }
            Events.push_back(std::move(event));
        }
        Condition.notify_all();
    }

    Phase0EnetWorker Worker;
    std::mutex Mutex;
    std::condition_variable Condition;
    std::deque<WorkerEvent> Events;
};

TransportHandle* FromHandle(jlong handle)
{
    return reinterpret_cast<TransportHandle*>(static_cast<std::uintptr_t>(handle));
}

jlong ToHandle(TransportHandle* handle)
{
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(handle));
}

void Put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

jbyteArray EncodeEvent(JNIEnv* env, WorkerEvent event)
{
    std::vector<std::uint8_t> encodedPacket;
    if (event.packet &&
        !Phase0ProtocolCodec::Encode(
            event.packet->envelope,
            event.packet->payload,
            encodedPacket))
        encodedPacket.clear();

    std::vector<std::uint8_t> bytes(EventHeaderSize + encodedPacket.size(), 0);
    bytes[0] = static_cast<std::uint8_t>(event.type);
    bytes[1] = static_cast<std::uint8_t>(event.error);
    bytes[2] = event.channel;
    Put32(bytes, 4, static_cast<std::uint32_t>(event.peerIndex));
    Put32(bytes, 8, static_cast<std::uint32_t>(encodedPacket.size()));
    std::copy(encodedPacket.begin(), encodedPacket.end(), bytes.begin() + EventHeaderSize);

    jbyteArray result = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (result != nullptr)
    {
        env->SetByteArrayRegion(
            result,
            0,
            static_cast<jsize>(bytes.size()),
            reinterpret_cast<const jbyte*>(bytes.data()));
    }
    return result;
}

}
}

using namespace MelonDSAndroid::Multiplayer;

extern "C"
{

JNIEXPORT jlong JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_createHostNative(
    JNIEnv*,
    jclass,
    jlong networkHandle,
    jint port,
    jint qualifiedMaxPlayers)
{
    if (networkHandle == 0 || port < 1024 || port > 65535 ||
        qualifiedMaxPlayers < 2 ||
        qualifiedMaxPlayers > InitialQualifiedMaxPlayers)
        return 0;
    WorkerSettings settings;
    settings.role = WorkerRole::Host;
    settings.networkHandle = static_cast<std::uint64_t>(networkHandle);
    settings.port = static_cast<std::uint16_t>(port);
    settings.qualifiedMaxPlayers = static_cast<std::uint8_t>(qualifiedMaxPlayers);
    return ToHandle(new TransportHandle(settings));
}

JNIEXPORT jlong JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_createClientNative(
    JNIEnv* env,
    jclass,
    jlong networkHandle,
    jint port,
    jbyteArray ipv4)
{
    if (networkHandle == 0 || port < 1024 || port > 65535 ||
        ipv4 == nullptr || env->GetArrayLength(ipv4) != 4)
        return 0;
    WorkerSettings settings;
    settings.role = WorkerRole::Client;
    settings.networkHandle = static_cast<std::uint64_t>(networkHandle);
    settings.port = static_cast<std::uint16_t>(port);
    env->GetByteArrayRegion(
        ipv4,
        0,
        4,
        reinterpret_cast<jbyte*>(settings.remoteIpv4.data()));
    return ToHandle(new TransportHandle(settings));
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_startNative(
    JNIEnv*,
    jclass,
    jlong handle)
{
    TransportHandle* transport = FromHandle(handle);
    return transport != nullptr && transport->Start();
}

JNIEXPORT jbyteArray JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_pollEventNative(
    JNIEnv* env,
    jclass,
    jlong handle,
    jint timeoutMilliseconds)
{
    TransportHandle* transport = FromHandle(handle);
    if (transport == nullptr)
        return nullptr;
    auto event = transport->Poll(std::chrono::milliseconds(
        std::max(0, timeoutMilliseconds)));
    return event ? EncodeEvent(env, std::move(*event)) : nullptr;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_sendNative(
    JNIEnv* env,
    jclass,
    jlong handle,
    jint peerIndex,
    jint channel,
    jint flags,
    jbyteArray packet)
{
    TransportHandle* transport = FromHandle(handle);
    if (transport == nullptr || peerIndex < 0 || channel < 0 || channel > 1 ||
        packet == nullptr)
        return false;
    const jsize length = env->GetArrayLength(packet);
    if (length <= 0 || length > static_cast<jsize>(MaximumApplicationPacketSize))
        return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    env->GetByteArrayRegion(
        packet,
        0,
        length,
        reinterpret_cast<jbyte*>(bytes.data()));
    return transport->Send(
        static_cast<std::size_t>(peerIndex),
        static_cast<std::uint8_t>(channel),
        static_cast<std::uint32_t>(flags),
        std::move(bytes));
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_markWelcomedNative(
    JNIEnv*,
    jclass,
    jlong handle,
    jint peerIndex)
{
    TransportHandle* transport = FromHandle(handle);
    return transport != nullptr && peerIndex >= 0 &&
        transport->MarkWelcomed(static_cast<std::size_t>(peerIndex));
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_stopNative(
    JNIEnv*,
    jclass,
    jlong handle)
{
    TransportHandle* transport = FromHandle(handle);
    return transport != nullptr && transport->StopAndWait(ShutdownSlo);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_awaitStoppedNative(
    JNIEnv*,
    jclass,
    jlong handle,
    jint timeoutMilliseconds)
{
    TransportHandle* transport = FromHandle(handle);
    return transport != nullptr &&
        transport->AwaitStopped(std::chrono::milliseconds(
            std::max(0, timeoutMilliseconds)));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_network_Phase0NativeTransport_destroyNative(
    JNIEnv*,
    jclass,
    jlong handle)
{
    delete FromHandle(handle);
}

}
