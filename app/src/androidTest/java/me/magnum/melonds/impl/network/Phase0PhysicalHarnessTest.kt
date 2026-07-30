package me.magnum.melonds.impl.network

import android.content.Context
import android.os.PowerManager
import android.util.Log
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import me.magnum.melonds.BuildConfig
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.BufferedWriter
import java.io.File
import java.net.Inet4Address
import java.net.InetAddress
import java.util.concurrent.TimeUnit

/**
 * Explicit adb-driven physical gate. It is never a normal CI/device test.
 *
 * Required instrumentation arguments are documented in the Phase 0 runbook.
 */
@RunWith(AndroidJUnit4::class)
class Phase0PhysicalHarnessTest {
    @Test
    fun runConfiguredPhysicalRole() {
        assumeTrue(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        val arguments = InstrumentationRegistry.getArguments()
        val role = arguments.getString("phase0_role")
        assumeTrue(
            "Set -e phase0_role to app_host, ds_host, or responder",
            role in setOf("app_host", "ds_host", "responder"),
        )

        val context = ApplicationProvider.getApplicationContext<Context>()
        val selected = requireNotNull(Phase0WifiNetworkProvider(context).select()) {
            "No selected IPv4 Wi-Fi Network"
        }
        val port = arguments.getString("phase0_port")?.toInt() ?: 7064
        val exchanges = arguments.getString("phase0_exchanges")?.toInt() ?: 10_000
        val warmupMilliseconds =
            arguments.getString("phase0_warmup_ms")?.toLong() ?: 60_000
        val outputId = arguments.getString("phase0_output_id")
            ?: "${role}-${System.currentTimeMillis()}"
        val aid = arguments.getString("phase0_aid")?.toInt() ?: 1
        val aidMask = arguments.getString("phase0_aid_mask")?.toInt() ?: (1 shl 1)
        val lowLatency = arguments.getString("phase0_low_latency")?.toBoolean() ?: false
        val playerName = arguments.getString("phase0_player_name") ?: role!!
        val lowLatencyController = Phase0LowLatencyWifiController(context)
        val powerManager = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        val lowLatencyResult = if (lowLatency) {
            lowLatencyController.acquireForMeasurement(
                applicationForeground = true,
                screenOn = powerManager.isInteractive,
            )
        } else {
            null
        }

        val outputDirectory = requireNotNull(context.getExternalFilesDir("phase0"))
        val summaryFile = File(outputDirectory, "$outputId-exchanges.csv")
        val traceFile = File(outputDirectory, "$outputId-trace.csv")
        summaryFile.bufferedWriter().use { summary ->
            traceFile.bufferedWriter().use { trace ->
                writeHeaders(summary, trace)
                summary.appendLine(
                    "# role=$role,network=${selected.interfaceName}," +
                        "ipv4=${selected.ipv4Address.toIpv4String()}," +
                        "api=${android.os.Build.VERSION.SDK_INT}," +
                        "device=${android.os.Build.MANUFACTURER}/${android.os.Build.MODEL}," +
                        "fingerprint=${android.os.Build.FINGERPRINT}," +
                        "lowLatency=$lowLatencyResult",
                )
                try {
                    when (role) {
                        "app_host" -> assertTrue(
                            Phase0CoreRelayHarness.startHost(
                                selected,
                                port,
                                playerName,
                                arguments.getString("phase0_session_name") ?: "Phase 0",
                                BuildConfig.VERSION_NAME,
                            ),
                        )
                        "ds_host", "responder" -> {
                            val hostAddress = requireNotNull(
                                arguments.getString("phase0_host_ipv4"),
                            ) { "Client role requires -e phase0_host_ipv4" }
                            val ipv4 = InetAddress.getByName(hostAddress)
                            require(ipv4 is Inet4Address)
                            assertTrue(
                                Phase0CoreRelayHarness.startClient(
                                    selected,
                                    port,
                                    ipv4.address,
                                    playerName,
                                    BuildConfig.VERSION_NAME,
                                ),
                            )
                        }
                    }

                    awaitLobby()
                    Thread.sleep(warmupMilliseconds)
                    if (role == "ds_host") {
                        runDsHost(exchanges, aidMask, summary, trace)
                    } else {
                        runResponder(exchanges, aid, summary, trace)
                    }
                } finally {
                    drainTrace(trace)
                    if (!Phase0CoreRelayHarness.stop()) {
                        assertTrue(Phase0CoreRelayHarness.awaitStopped(30_000))
                    }
                    lowLatencyController.close()
                }
            }
        }
        Log.i(TAG, "Phase 0 summary: ${summaryFile.absolutePath}")
        Log.i(TAG, "Phase 0 trace: ${traceFile.absolutePath}")
    }

    private fun runDsHost(
        exchanges: Int,
        aidMask: Int,
        summary: BufferedWriter,
        trace: BufferedWriter,
    ) {
        repeat(exchanges) { index ->
            val result = requireNotNull(
                Phase0CoreRelayHarness.runDsExchange(
                    frameLength = if (index % 100 == 0) 0x948 else 36,
                    associationIdMask = aidMask,
                ),
            )
            summary.appendLine(
                "$index,${result.commandBytesSent},${result.replyAidMask}," +
                    "${result.ackBytesSent},${result.elapsedNanoseconds}",
            )
            if ((index + 1) % TRACE_DRAIN_INTERVAL == 0) {
                summary.flush()
                drainTrace(trace)
            }
        }
    }

    private fun runResponder(
        exchanges: Int,
        aid: Int,
        summary: BufferedWriter,
        trace: BufferedWriter,
    ) {
        var completed = 0
        val deadline = System.nanoTime() +
            TimeUnit.MILLISECONDS.toNanos(exchanges.toLong() * 100L + 30_000L)
        while (completed < exchanges && System.nanoTime() < deadline) {
            val result = requireNotNull(Phase0CoreRelayHarness.runResponderOnce(aid))
            if (result.replyBytesSent > 0) {
                summary.appendLine(
                    "$completed,${result.commandBytesReceived},0," +
                        "${result.replyBytesSent},0",
                )
                ++completed
                if (completed % TRACE_DRAIN_INTERVAL == 0) {
                    summary.flush()
                    drainTrace(trace)
                }
            }
        }
        assertTrue("Responder completed $completed/$exchanges exchanges", completed == exchanges)
    }

    private fun awaitLobby() {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10)
        while (System.nanoTime() < deadline) {
            when (Phase0CoreRelayHarness.state()) {
                Phase0RuntimeState.LOBBY -> return
                Phase0RuntimeState.FAILED ->
                    throw AssertionError("Phase 0 runtime failed: ${Phase0CoreRelayHarness.error()}")
                else -> Thread.sleep(25)
            }
        }
        throw AssertionError("Timed out waiting for Phase 0 lobby")
    }

    private fun drainTrace(writer: BufferedWriter) {
        val batch = Phase0CoreRelayHarness.drainTrace()
        batch.events.forEach { event ->
            writer.appendLine(
                "${event.monotonicNanoseconds},${event.emulatedTimestamp}," +
                    "${event.sequence},${event.value},${event.type}," +
                    "${event.playerId},${event.peerIndex},${batch.droppedEvents}",
            )
        }
        writer.flush()
    }

    private fun writeHeaders(summary: BufferedWriter, trace: BufferedWriter) {
        summary.appendLine(
            "sample,command_bytes,reply_aid_mask,ack_or_reply_bytes,elapsed_ns",
        )
        trace.appendLine(
            "monotonic_ns,emulated_timestamp,sequence,value,type,player_id,peer_index,dropped",
        )
    }

    private fun ByteArray.toIpv4String(): String =
        joinToString(".") { (it.toInt() and 0xFF).toString() }

    companion object {
        private const val TAG = "Phase0PhysicalHarness"
        private const val TRACE_DRAIN_INTERVAL = 500
    }
}
