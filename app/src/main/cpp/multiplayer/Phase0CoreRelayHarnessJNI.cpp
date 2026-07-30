#include "Phase0SessionRuntime.h"
#include "Phase0TraceRecorder.h"

#include <MPInterface.h>

#include <jni.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace MelonDSAndroid::Multiplayer;

namespace
{

std::mutex RuntimeMutex;
std::unique_ptr<Phase0SessionRuntime> Runtime;

class FacadeProbeBackend final : public melonDS::MPInterface
{
public:
    explicit FacadeProbeBackend(std::atomic_bool& replayed)
        : Replayed(replayed)
    {
    }

    void Process() override {}
    void Begin(int instance) override
    {
        if (instance == 0 && GetRecvTimeout() == 37)
            Replayed.store(true);
    }
    void End(int) override {}
    int SendPacket(int, melonDS::u8*, int length, melonDS::u64) override
    {
        return length;
    }
    int RecvPacket(int, melonDS::u8*, melonDS::u64*) override { return 0; }
    int SendCmd(int, melonDS::u8*, int length, melonDS::u64) override
    {
        return length;
    }
    int SendReply(
        int,
        melonDS::u8*,
        int length,
        melonDS::u64,
        melonDS::u16) override
    {
        return length;
    }
    int SendAck(int, melonDS::u8*, int length, melonDS::u64) override
    {
        return length;
    }
    int RecvHostPacket(int, melonDS::u8*, melonDS::u64*) override { return 0; }
    melonDS::u16 RecvReplies(
        int,
        melonDS::u8*,
        melonDS::u64,
        melonDS::u16) override
    {
        return 0;
    }

private:
    std::atomic_bool& Replayed;
};

std::string StringFromJava(JNIEnv* env, jstring value)
{
    if (value == nullptr)
        return {};
    const char* bytes = env->GetStringUTFChars(value, nullptr);
    if (bytes == nullptr)
        return {};
    std::string result(bytes);
    env->ReleaseStringUTFChars(value, bytes);
    return result;
}

void Put16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

void Put64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

std::vector<melonDS::u8> SyntheticFrame(std::size_t requestedLength)
{
    const std::size_t length = std::clamp<std::size_t>(requestedLength, 36, 0x948);
    std::vector<melonDS::u8> frame(length, 0);
    const auto internalLength = static_cast<std::uint16_t>(length - 12);
    frame[10] = static_cast<melonDS::u8>(internalLength);
    frame[11] = static_cast<melonDS::u8>(internalLength >> 8);
    return frame;
}

bool StartRuntime(RuntimeSettings settings)
{
    std::lock_guard lock(RuntimeMutex);
    if (Runtime)
        return false;
    Runtime = Phase0SessionRuntime::Create(std::move(settings));
    if (!Runtime)
        return false;
    // The content-free harness models one active core endpoint. On a real
    // emulator run Wifi::SetPowerControl supplies this Begin instead.
    melonDS::MPInterface::Get().Begin(0);
    if (!Runtime->Start())
    {
        melonDS::MPInterface::Get().End(0);
        Runtime.reset();
        return false;
    }
    return true;
}

}

extern "C"
{

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_startHostNative(
    JNIEnv* env,
    jclass,
    jlong networkHandle,
    jint port,
    jstring playerName,
    jstring sessionName,
    jstring buildId)
{
    RuntimeSettings settings;
    settings.transport.role = WorkerRole::Host;
    settings.transport.networkHandle = static_cast<std::uint64_t>(networkHandle);
    settings.transport.port = static_cast<std::uint16_t>(port);
    settings.transport.qualifiedMaxPlayers = InitialQualifiedMaxPlayers;
    settings.playerName = StringFromJava(env, playerName);
    settings.sessionName = StringFromJava(env, sessionName);
    settings.buildId = StringFromJava(env, buildId);
    return StartRuntime(std::move(settings));
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_startClientNative(
    JNIEnv* env,
    jclass,
    jlong networkHandle,
    jint port,
    jbyteArray ipv4,
    jstring playerName,
    jstring buildId)
{
    if (ipv4 == nullptr || env->GetArrayLength(ipv4) != 4)
        return false;
    RuntimeSettings settings;
    settings.transport.role = WorkerRole::Client;
    settings.transport.networkHandle = static_cast<std::uint64_t>(networkHandle);
    settings.transport.port = static_cast<std::uint16_t>(port);
    env->GetByteArrayRegion(
        ipv4,
        0,
        4,
        reinterpret_cast<jbyte*>(settings.transport.remoteIpv4.data()));
    settings.playerName = StringFromJava(env, playerName);
    settings.buildId = StringFromJava(env, buildId);
    return StartRuntime(std::move(settings));
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_stateNative(
    JNIEnv*,
    jclass)
{
    std::lock_guard lock(RuntimeMutex);
    return Runtime
        ? static_cast<jint>(Runtime->State())
        : static_cast<jint>(RuntimeState::Stopped);
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_errorNative(
    JNIEnv*,
    jclass)
{
    std::lock_guard lock(RuntimeMutex);
    return Runtime
        ? static_cast<jint>(Runtime->Error())
        : static_cast<jint>(WorkerError::None);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_stopNative(
    JNIEnv*,
    jclass)
{
    std::lock_guard lock(RuntimeMutex);
    if (!Runtime)
        return true;
    melonDS::MPInterface::Get().End(0);
    if (!Runtime->Stop())
        return false;
    Runtime.reset();
    return true;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_awaitStoppedNative(
    JNIEnv*,
    jclass,
    jint timeoutMilliseconds)
{
    std::lock_guard lock(RuntimeMutex);
    if (!Runtime)
        return true;
    if (!Runtime->AwaitStopped(std::chrono::milliseconds(
            std::max(0, timeoutMilliseconds))))
        return false;
    Runtime.reset();
    return true;
}

JNIEXPORT jlongArray JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_runDsExchangeNative(
    JNIEnv* env,
    jclass,
    jint frameLength,
    jint associationIdMask)
{
    {
        std::lock_guard lock(RuntimeMutex);
        if (!Runtime || Runtime->State() != RuntimeState::Lobby)
            return nullptr;
    }
    auto command = SyntheticFrame(static_cast<std::size_t>(frameLength));
    auto ack = SyntheticFrame(44);
    std::array<melonDS::u8, 15 * 1024> replies{};
    const auto started = std::chrono::steady_clock::now();
    const int sent = melonDS::MPInterface::Get().SendCmd(
        0,
        command.data(),
        static_cast<int>(command.size()),
        1);
    const melonDS::u16 replyMask = sent == 0
        ? 0
        : melonDS::MPInterface::Get().RecvReplies(
            0,
            replies.data(),
            1,
            static_cast<melonDS::u16>(associationIdMask));
    const int ackSent = sent == 0
        ? 0
        : melonDS::MPInterface::Get().SendAck(
            0,
            ack.data(),
            static_cast<int>(ack.size()),
            1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (elapsed >= std::chrono::milliseconds(25).count() * 1000000LL)
    {
        Phase0TraceRecorder::Get().Record(
            TraceEventType::DeadlineMiss,
            0,
            1,
            static_cast<std::uint32_t>(
                std::min<std::int64_t>(elapsed / 1000, UINT32_MAX)));
    }
    const jlong values[] = {
        sent,
        replyMask,
        ackSent,
        elapsed,
    };
    jlongArray result = env->NewLongArray(4);
    if (result != nullptr)
        env->SetLongArrayRegion(result, 0, 4, values);
    return result;
}

JNIEXPORT jlongArray JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_runResponderOnceNative(
    JNIEnv* env,
    jclass,
    jint associationId)
{
    {
        std::lock_guard lock(RuntimeMutex);
        if (!Runtime || Runtime->State() != RuntimeState::Lobby)
            return nullptr;
    }
    std::array<melonDS::u8, 0x948> command{};
    melonDS::u64 timestamp = 0;
    const int received = melonDS::MPInterface::Get().RecvHostPacket(
        0,
        command.data(),
        &timestamp);
    int sent = 0;
    if (received > 0)
    {
        auto reply = SyntheticFrame(
            std::min<std::size_t>(static_cast<std::size_t>(received), 1024));
        sent = melonDS::MPInterface::Get().SendReply(
            0,
            reply.data(),
            static_cast<int>(reply.size()),
            timestamp,
            static_cast<melonDS::u16>(associationId));
    }
    const jlong values[] = {received, sent, static_cast<jlong>(timestamp)};
    jlongArray result = env->NewLongArray(3);
    if (result != nullptr)
        env->SetLongArrayRegion(result, 0, 3, values);
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_drainTraceNative(
    JNIEnv* env,
    jclass)
{
    constexpr std::size_t headerSize = 12;
    constexpr std::size_t eventSize = 28;
    const std::vector<TraceEvent> events = Phase0TraceRecorder::Get().Drain();
    std::vector<std::uint8_t> encoded(headerSize + eventSize * events.size(), 0);
    Put32(encoded, 0, static_cast<std::uint32_t>(events.size()));
    Put64(encoded, 4, Phase0TraceRecorder::Get().Dropped());
    std::size_t offset = headerSize;
    for (const TraceEvent& event : events)
    {
        Put64(encoded, offset, event.monotonicNanoseconds);
        Put64(encoded, offset + 8, event.emulatedTimestamp);
        Put32(encoded, offset + 16, event.sequence);
        Put32(encoded, offset + 20, event.value);
        Put16(encoded, offset + 24, event.type);
        encoded[offset + 26] = event.playerId;
        encoded[offset + 27] = event.peerIndex;
        offset += eventSize;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(encoded.size()));
    if (result != nullptr)
    {
        env->SetByteArrayRegion(
            result,
            0,
            static_cast<jsize>(encoded.size()),
            reinterpret_cast<const jbyte*>(encoded.data()));
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_network_Phase0CoreRelayHarness_runFacadeSwitchStressNative(
    JNIEnv*,
    jclass,
    jint iterations)
{
    std::lock_guard runtimeLock(RuntimeMutex);
    if (Runtime || iterations <= 0)
        return false;
    std::atomic_bool running{true};
    std::atomic_bool replayed{false};
    std::array<melonDS::u8, 36> frame{};
    frame[10] = 24;
    melonDS::MPInterface::Get().SetRecvTimeout(37);
    melonDS::MPInterface::Get().Begin(0);
    std::thread caller([&] {
        while (running.load())
            melonDS::MPInterface::Get().SendPacket(0, frame.data(), frame.size(), 0);
    });
    for (int index = 0; index < iterations; ++index)
    {
        replayed.store(false);
        melonDS::MPInterface::Install(
            std::make_unique<FacadeProbeBackend>(replayed),
            melonDS::MPInterface_Phase0LAN);
        if (!replayed.load() ||
            melonDS::MPInterface::GetActiveInstanceMask() != 1)
        {
            running.store(false);
            caller.join();
            melonDS::MPInterface::Get().End(0);
            melonDS::MPInterface::Set(melonDS::MPInterface_Local);
            return false;
        }
    }
    running.store(false);
    caller.join();
    melonDS::MPInterface::Get().End(0);
    melonDS::MPInterface::Set(melonDS::MPInterface_Local);
    return melonDS::MPInterface::GetActiveInstanceMask() == 0;
}

}
