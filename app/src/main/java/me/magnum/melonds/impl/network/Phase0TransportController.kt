package me.magnum.melonds.impl.network

import java.io.Closeable
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicReference

enum class Phase0ControllerState {
    IDLE,
    STARTING,
    RUNNING,
    STOPPING,
    FAILED,
}

data class Phase0ControllerSnapshot(
    val state: Phase0ControllerState,
    val error: Phase0ControllerError? = null,
)

enum class Phase0ControllerError {
    INVALID_STATE,
    START_FAILED,
    WORKER_FAILED,
    SHUTDOWN_TIMEOUT,
}

/**
 * Minimal retained Phase 0 lifecycle guard.
 *
 * A shutdown SLO breach remains STOPPING with the old transport retained.
 * Host/join retry is impossible until worker-owned cleanup completes and the
 * controller publishes FAILED(SHUTDOWN_TIMEOUT).
 */
class Phase0TransportController : Closeable {
    private val snapshot = AtomicReference(
        Phase0ControllerSnapshot(Phase0ControllerState.IDLE),
    )
    private val cleanupExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "Phase0TransportCleanup").apply { isDaemon = true }
    }
    private val eventExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "Phase0TransportEvents").apply { isDaemon = true }
    }

    @Volatile
    private var transport: Phase0NativeTransport? = null

    fun snapshot(): Phase0ControllerSnapshot = snapshot.get()

    @Synchronized
    fun start(candidate: Phase0NativeTransport): Boolean {
        val current = snapshot.get().state
        if (transport != null ||
            (current != Phase0ControllerState.IDLE &&
                current != Phase0ControllerState.FAILED)
        ) {
            snapshot.set(
                Phase0ControllerSnapshot(current, Phase0ControllerError.INVALID_STATE),
            )
            return false
        }
        transport = candidate
        snapshot.set(Phase0ControllerSnapshot(Phase0ControllerState.STARTING))
        if (!candidate.start()) {
            candidate.close()
            transport = null
            snapshot.set(
                Phase0ControllerSnapshot(
                    Phase0ControllerState.FAILED,
                    Phase0ControllerError.START_FAILED,
                ),
            )
            return false
        }
        eventExecutor.execute { pumpEvents(candidate) }
        return true
    }

    @Synchronized
    fun stop(failAfterCleanup: Boolean = false) {
        val active = transport
        if (active == null) {
            snapshot.set(
                Phase0ControllerSnapshot(
                    if (failAfterCleanup) {
                        Phase0ControllerState.FAILED
                    } else {
                        Phase0ControllerState.IDLE
                    },
                ),
            )
            return
        }
        if (snapshot.get().state == Phase0ControllerState.STOPPING) return

        snapshot.set(Phase0ControllerSnapshot(Phase0ControllerState.STOPPING))
        if (active.stop()) {
            finishCleanup(active, failAfterCleanup, null)
            return
        }

        snapshot.set(
            Phase0ControllerSnapshot(
                Phase0ControllerState.STOPPING,
                Phase0ControllerError.SHUTDOWN_TIMEOUT,
            ),
        )
        cleanupExecutor.execute {
            active.awaitStopped(Int.MAX_VALUE)
            finishCleanup(
                active,
                true,
                Phase0ControllerError.SHUTDOWN_TIMEOUT,
            )
        }
    }

    override fun close() {
        stop()
        cleanupExecutor.shutdown()
        eventExecutor.shutdown()
    }

    private fun pumpEvents(candidate: Phase0NativeTransport) {
        while (transport === candidate) {
            val event = candidate.pollEvent(100) ?: continue
            when (event.type) {
                Phase0WorkerEventType.STARTED -> snapshot.updateAndGet {
                    if (it.state == Phase0ControllerState.STARTING) {
                        Phase0ControllerSnapshot(Phase0ControllerState.RUNNING)
                    } else {
                        it
                    }
                }
                Phase0WorkerEventType.FAILURE -> {
                    if (snapshot.get().state != Phase0ControllerState.STOPPING) {
                        stop(failAfterCleanup = true)
                    }
                }
                Phase0WorkerEventType.STOPPED -> return
                else -> Unit
            }
        }
    }

    @Synchronized
    private fun finishCleanup(
        candidate: Phase0NativeTransport,
        failed: Boolean,
        error: Phase0ControllerError?,
    ) {
        if (transport !== candidate) return
        candidate.close()
        transport = null
        snapshot.set(
            Phase0ControllerSnapshot(
                if (failed) Phase0ControllerState.FAILED else Phase0ControllerState.IDLE,
                error ?: if (failed) Phase0ControllerError.WORKER_FAILED else null,
            ),
        )
    }
}
