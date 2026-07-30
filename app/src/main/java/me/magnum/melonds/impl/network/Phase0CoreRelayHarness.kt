package me.magnum.melonds.impl.network

import me.magnum.melonds.BuildConfig
import java.nio.ByteBuffer
import java.nio.ByteOrder

enum class Phase0RuntimeState {
    STARTING,
    JOINING,
    LOBBY,
    STOPPING,
    FAILED,
    STOPPED,
}

data class Phase0ExchangeResult(
    val commandBytesSent: Int,
    val replyAidMask: Int,
    val ackBytesSent: Int,
    val elapsedNanoseconds: Long,
)

data class Phase0ResponderResult(
    val commandBytesReceived: Int,
    val replyBytesSent: Int,
    val emulatedTimestamp: Long,
)

data class Phase0TraceEvent(
    val monotonicNanoseconds: Long,
    val emulatedTimestamp: Long,
    val sequence: Long,
    val value: Long,
    val type: Int,
    val playerId: Int,
    val peerIndex: Int,
)

data class Phase0TraceBatch(
    val droppedEvents: Long,
    val events: List<Phase0TraceEvent>,
)

/**
 * Content-free driver for the real MPInterface adapter and selected-network
 * ENet path. This is intentionally not connected to production lobby UI.
 */
object Phase0CoreRelayHarness {
    fun startHost(
        selected: Phase0SelectedWifiNetwork,
        port: Int,
        playerName: String,
        sessionName: String,
        buildId: String,
    ): Boolean {
        check(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        return startHostNative(
            selected.networkHandle,
            port,
            playerName,
            sessionName,
            buildId,
        )
    }

    fun startClient(
        selected: Phase0SelectedWifiNetwork,
        port: Int,
        hostIpv4: ByteArray,
        playerName: String,
        buildId: String,
    ): Boolean {
        check(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        require(hostIpv4.size == 4)
        return startClientNative(
            selected.networkHandle,
            port,
            hostIpv4,
            playerName,
            buildId,
        )
    }

    fun state(): Phase0RuntimeState = Phase0RuntimeState.entries[stateNative()]

    fun error(): Phase0WorkerError = Phase0WorkerError.entries[errorNative()]

    fun runDsExchange(
        frameLength: Int = 36,
        associationIdMask: Int = 1 shl 1,
    ): Phase0ExchangeResult? = runDsExchangeNative(
        frameLength,
        associationIdMask,
    )?.let {
        Phase0ExchangeResult(
            commandBytesSent = it[0].toInt(),
            replyAidMask = it[1].toInt(),
            ackBytesSent = it[2].toInt(),
            elapsedNanoseconds = it[3],
        )
    }

    fun runResponderOnce(associationId: Int = 1): Phase0ResponderResult? =
        runResponderOnceNative(associationId)?.let {
            Phase0ResponderResult(
                commandBytesReceived = it[0].toInt(),
                replyBytesSent = it[1].toInt(),
                emulatedTimestamp = it[2],
            )
        }

    fun drainTrace(): Phase0TraceBatch {
        val buffer = ByteBuffer.wrap(drainTraceNative()).order(ByteOrder.LITTLE_ENDIAN)
        val count = buffer.int
        val dropped = buffer.long
        val events = ArrayList<Phase0TraceEvent>(count)
        repeat(count) {
            events += Phase0TraceEvent(
                monotonicNanoseconds = buffer.long,
                emulatedTimestamp = buffer.long,
                sequence = buffer.int.toLong() and 0xFFFF_FFFFL,
                value = buffer.int.toLong() and 0xFFFF_FFFFL,
                type = buffer.short.toInt() and 0xFFFF,
                playerId = buffer.get().toInt() and 0xFF,
                peerIndex = buffer.get().toInt() and 0xFF,
            )
        }
        check(!buffer.hasRemaining())
        return Phase0TraceBatch(dropped, events)
    }

    fun stop(): Boolean = stopNative()

    fun awaitStopped(timeoutMilliseconds: Int): Boolean =
        awaitStoppedNative(timeoutMilliseconds)

    fun runFacadeSwitchStress(iterations: Int = 1_000): Boolean {
        check(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        require(iterations > 0)
        return runFacadeSwitchStressNative(iterations)
    }

    @JvmStatic private external fun startHostNative(
        networkHandle: Long,
        port: Int,
        playerName: String,
        sessionName: String,
        buildId: String,
    ): Boolean
    @JvmStatic private external fun startClientNative(
        networkHandle: Long,
        port: Int,
        ipv4: ByteArray,
        playerName: String,
        buildId: String,
    ): Boolean
    @JvmStatic private external fun stateNative(): Int
    @JvmStatic private external fun errorNative(): Int
    @JvmStatic private external fun runDsExchangeNative(
        frameLength: Int,
        associationIdMask: Int,
    ): LongArray?
    @JvmStatic private external fun runResponderOnceNative(
        associationId: Int,
    ): LongArray?
    @JvmStatic private external fun drainTraceNative(): ByteArray
    @JvmStatic private external fun stopNative(): Boolean
    @JvmStatic private external fun awaitStoppedNative(
        timeoutMilliseconds: Int,
    ): Boolean
    @JvmStatic private external fun runFacadeSwitchStressNative(
        iterations: Int,
    ): Boolean
}
