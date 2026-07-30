package me.magnum.melonds.impl.network

import me.magnum.melonds.BuildConfig
import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.ByteOrder

enum class Phase0WorkerEventType {
    STARTED,
    CONNECTED,
    PACKET,
    DISCONNECTED,
    FAILURE,
    STOPPED,
}

enum class Phase0WorkerError {
    NONE,
    ENET_INITIALIZATION_FAILED,
    HOST_CREATION_FAILED,
    NETWORK_BIND_FAILED,
    PORT_UNAVAILABLE,
    ADDRESS_INVALID,
    CONNECT_FAILED,
    PROTOCOL_VIOLATION,
    PEER_HELLO_TIMEOUT,
    QUEUE_FULL,
    RELIABLE_BUDGET_EXCEEDED,
    WORKER_FAILED,
}

data class Phase0WorkerEvent(
    val type: Phase0WorkerEventType,
    val error: Phase0WorkerError,
    val channel: Int,
    val peerIndex: Int,
    val packet: ByteArray,
)

/**
 * Polling-only JNI surface for the non-production physical feasibility slice.
 *
 * Native retains no Activity, callback, JNIEnv, or Java object. The complete
 * session/controller API remains gated behind the Phase 0 topology decision.
 */
class Phase0NativeTransport private constructor(
    private var nativeHandle: Long,
) : Closeable {
    fun start(): Boolean = startNative(nativeHandle)

    fun pollEvent(timeoutMilliseconds: Int): Phase0WorkerEvent? {
        val bytes = pollEventNative(nativeHandle, timeoutMilliseconds) ?: return null
        require(bytes.size >= EVENT_HEADER_SIZE)
        val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val type = Phase0WorkerEventType.entries[buffer.get().toInt()]
        val error = Phase0WorkerError.entries[buffer.get().toInt()]
        val channel = buffer.get().toInt() and 0xFF
        buffer.get()
        val peerIndex = buffer.int
        val packetLength = buffer.int
        require(packetLength >= 0 && packetLength == bytes.size - EVENT_HEADER_SIZE)
        val packet = ByteArray(packetLength)
        buffer.get(packet)
        return Phase0WorkerEvent(type, error, channel, peerIndex, packet)
    }

    fun send(peerIndex: Int, channel: Int, flags: Int, packet: ByteArray): Boolean =
        sendNative(nativeHandle, peerIndex, channel, flags, packet)

    fun markWelcomed(peerIndex: Int): Boolean =
        markWelcomedNative(nativeHandle, peerIndex)

    /**
     * Returns false on a 2-second SLO breach. The handle remains alive and
     * must be awaited; native ENet state is never destroyed cross-thread.
     */
    fun stop(): Boolean = stopNative(nativeHandle)

    fun awaitStopped(timeoutMilliseconds: Int): Boolean =
        awaitStoppedNative(nativeHandle, timeoutMilliseconds)

    override fun close() {
        val handle = nativeHandle
        if (handle == 0L) return
        if (!stopNative(handle)) {
            check(awaitStoppedNative(handle, Int.MAX_VALUE)) {
                "Phase 0 ENet worker did not complete worker-owned cleanup"
            }
        }
        destroyNative(handle)
        nativeHandle = 0
    }

    companion object {
        const val ENET_PACKET_FLAG_RELIABLE = 1
        const val ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT = 1 shl 3
        private const val EVENT_HEADER_SIZE = 12

        fun createHost(
            selectedNetwork: Phase0SelectedWifiNetwork,
            port: Int,
            qualifiedMaxPlayers: Int = 4,
        ): Phase0NativeTransport {
            check(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
            val handle = createHostNative(
                selectedNetwork.networkHandle,
                port,
                qualifiedMaxPlayers,
            )
            check(handle != 0L)
            return Phase0NativeTransport(handle)
        }

        fun createClient(
            selectedNetwork: Phase0SelectedWifiNetwork,
            port: Int,
            ipv4: ByteArray,
        ): Phase0NativeTransport {
            check(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
            require(ipv4.size == 4)
            val handle = createClientNative(selectedNetwork.networkHandle, port, ipv4)
            check(handle != 0L)
            return Phase0NativeTransport(handle)
        }

        @JvmStatic
        private external fun createHostNative(
            networkHandle: Long,
            port: Int,
            qualifiedMaxPlayers: Int,
        ): Long

        @JvmStatic
        private external fun createClientNative(
            networkHandle: Long,
            port: Int,
            ipv4: ByteArray,
        ): Long

        @JvmStatic private external fun startNative(handle: Long): Boolean
        @JvmStatic private external fun pollEventNative(
            handle: Long,
            timeoutMilliseconds: Int,
        ): ByteArray?
        @JvmStatic private external fun sendNative(
            handle: Long,
            peerIndex: Int,
            channel: Int,
            flags: Int,
            packet: ByteArray,
        ): Boolean
        @JvmStatic private external fun markWelcomedNative(handle: Long, peerIndex: Int): Boolean
        @JvmStatic private external fun stopNative(handle: Long): Boolean
        @JvmStatic private external fun awaitStoppedNative(
            handle: Long,
            timeoutMilliseconds: Int,
        ): Boolean
        @JvmStatic private external fun destroyNative(handle: Long)
    }
}
