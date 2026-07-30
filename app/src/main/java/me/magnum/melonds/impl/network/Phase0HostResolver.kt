package me.magnum.melonds.impl.network

import android.net.Network
import java.io.Closeable
import java.net.Inet4Address
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.RejectedExecutionException
import java.util.concurrent.ThreadFactory
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import javax.inject.Inject
import javax.inject.Singleton

sealed interface Phase0ResolveAdmission {
    data object Accepted : Phase0ResolveAdmission
    data object QueueFull : Phase0ResolveAdmission
    data object Stopping : Phase0ResolveAdmission
}

sealed interface Phase0ResolveResult {
    data class Success(val ipv4Addresses: List<ByteArray>) : Phase0ResolveResult
    data object NoIpv4 : Phase0ResolveResult
    data class Failed(val cause: Throwable) : Phase0ResolveResult
}

/**
 * Selected-network resolver with four total executing/queued calls.
 *
 * Cancellation is logical. A stale generation's blocking getAllByName call is
 * allowed to return, remains charged against capacity until then, and its
 * result is discarded rather than touching a newer native session.
 */
@Singleton
class Phase0HostResolver internal constructor(
    private val lookup: (Network, String) -> Array<java.net.InetAddress>,
) : Closeable {
    @Inject
    constructor() : this({ network, hostname -> network.getAllByName(hostname) })

    private val generation = AtomicLong(0)
    private val admittedCalls = AtomicInteger(0)
    private val stopping = AtomicBoolean(false)
    private val executor = ThreadPoolExecutor(
        2,
        2,
        0,
        TimeUnit.MILLISECONDS,
        ArrayBlockingQueue(2),
        ThreadFactory { runnable ->
            Thread(runnable, "Phase0NetworkResolver").apply { isDaemon = true }
        },
        ThreadPoolExecutor.AbortPolicy(),
    )

    fun beginGeneration(): Long = generation.incrementAndGet()

    fun cancel(resolverGeneration: Long) {
        generation.compareAndSet(resolverGeneration, resolverGeneration + 1)
    }

    fun resolve(
        network: Network,
        hostname: String,
        resolverGeneration: Long,
        callback: (Phase0ResolveResult) -> Unit,
    ): Phase0ResolveAdmission {
        if (stopping.get()) return Phase0ResolveAdmission.Stopping
        if (!reserveCapacity()) return Phase0ResolveAdmission.QueueFull

        try {
            executor.execute {
                val result = try {
                    val addresses = lookup(network, hostname)
                        .asSequence()
                        .filterIsInstance<Inet4Address>()
                        .map { it.address.copyOf() }
                        .toList()
                    if (addresses.isEmpty()) {
                        Phase0ResolveResult.NoIpv4
                    } else {
                        Phase0ResolveResult.Success(addresses)
                    }
                } catch (error: Throwable) {
                    Phase0ResolveResult.Failed(error)
                } finally {
                    admittedCalls.decrementAndGet()
                }

                if (!stopping.get() && generation.get() == resolverGeneration) {
                    callback(result)
                }
            }
        } catch (_: RejectedExecutionException) {
            admittedCalls.decrementAndGet()
            return if (stopping.get()) {
                Phase0ResolveAdmission.Stopping
            } else {
                Phase0ResolveAdmission.QueueFull
            }
        }
        return Phase0ResolveAdmission.Accepted
    }

    override fun close() {
        if (stopping.compareAndSet(false, true)) {
            generation.incrementAndGet()
            executor.shutdown()
        }
    }

    private fun reserveCapacity(): Boolean {
        while (true) {
            val current = admittedCalls.get()
            if (current >= MAX_EXECUTING_OR_QUEUED) return false
            if (admittedCalls.compareAndSet(current, current + 1)) return true
        }
    }

    companion object {
        const val MAX_EXECUTING_OR_QUEUED = 4
    }
}
