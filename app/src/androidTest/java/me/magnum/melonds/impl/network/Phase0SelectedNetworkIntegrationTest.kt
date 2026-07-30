package me.magnum.melonds.impl.network

import android.content.Context
import android.net.ConnectivityManager
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import me.magnum.melonds.BuildConfig
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeNotNull
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.net.InetAddress
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

@RunWith(AndroidJUnit4::class)
class Phase0SelectedNetworkIntegrationTest {
    @Test
    fun stableFacadeQuiescesAndReplaysActiveInstanceDuringRepeatedSwitches() {
        assumeTrue(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        assertTrue(Phase0CoreRelayHarness.runFacadeSwitchStress(1_000))
    }

    @Test
    fun selectedWifiSupportsBoundInboundOutboundAndPortCollisionCleanup() {
        assumeTrue(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        val selected = Phase0WifiNetworkProvider(
            ApplicationProvider.getApplicationContext(),
        ).select()
        assumeNotNull(selected)
        requireNotNull(selected)
        assertEquals(4, selected.ipv4Address.size)
        assertTrue(selected.networkHandle != 0L)

        val port = 57064
        Phase0NativeTransport.createHost(selected, port).use { host ->
            assertTrue(host.start())
            assertEquals(
                Phase0WorkerEventType.STARTED,
                host.await(Phase0WorkerEventType.STARTED).type,
            )

            Phase0NativeTransport.createHost(selected, port).use { collision ->
                assertTrue(collision.start())
                assertEquals(
                    Phase0WorkerError.PORT_UNAVAILABLE,
                    collision.await(Phase0WorkerEventType.FAILURE).error,
                )
                assertEquals(
                    Phase0WorkerEventType.STOPPED,
                    collision.await(Phase0WorkerEventType.STOPPED).type,
                )
            }

            Phase0NativeTransport.createClient(
                selected,
                port,
                selected.ipv4Address,
            ).use { client ->
                assertTrue(client.start())
                val clientConnected = client.await(Phase0WorkerEventType.CONNECTED)
                val hostConnected = host.await(Phase0WorkerEventType.CONNECTED)
                assertTrue(host.markWelcomed(hostConnected.peerIndex))
                assertEquals(0, clientConnected.peerIndex)
                assertTrue(client.stop())
            }
            assertTrue(host.stop())
        }
    }

    @Test
    fun selectedWifiStillCarriesEnetWhenVpnOrAnotherNetworkIsDefault() {
        assumeTrue(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        val context = ApplicationProvider.getApplicationContext<Context>()
        val selected = Phase0WifiNetworkProvider(context).select()
        assumeNotNull(selected)
        requireNotNull(selected)
        val connectivityManager =
            context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        assumeTrue(connectivityManager.activeNetwork != selected.network)

        val port = 57065
        Phase0NativeTransport.createHost(selected, port).use { host ->
            assertTrue(host.start())
            assertEquals(
                Phase0WorkerEventType.STARTED,
                host.await(Phase0WorkerEventType.STARTED).type,
            )
            Phase0NativeTransport.createClient(
                selected,
                port,
                selected.ipv4Address,
            ).use { client ->
                assertTrue(client.start())
                assertEquals(
                    Phase0WorkerEventType.CONNECTED,
                    client.await(Phase0WorkerEventType.CONNECTED).type,
                )
                val hostConnected = host.await(Phase0WorkerEventType.CONNECTED)
                assertTrue(host.markWelcomed(hostConnected.peerIndex))
            }
        }
    }

    @Test
    fun selectedNetworkResolverReportsNoIpv4AndDiscardsCancelledGeneration() {
        assumeTrue(BuildConfig.PHASE0_MULTIPLAYER_ENABLED)
        val selected = Phase0WifiNetworkProvider(
            ApplicationProvider.getApplicationContext(),
        ).select()
        assumeNotNull(selected)
        requireNotNull(selected)

        val ipv6Only = InetAddress.getByAddress(ByteArray(16) { 1 })
        Phase0HostResolver { _, _ -> arrayOf(ipv6Only) }.use { resolver ->
            val result = AtomicReference<Phase0ResolveResult>()
            val completed = CountDownLatch(1)
            val generation = resolver.beginGeneration()
            assertEquals(
                Phase0ResolveAdmission.Accepted,
                resolver.resolve(selected.network, "ipv6-only.invalid", generation) {
                    result.set(it)
                    completed.countDown()
                },
            )
            assertTrue(completed.await(2, TimeUnit.SECONDS))
            assertEquals(Phase0ResolveResult.NoIpv4, result.get())

            val staleCallback = CountDownLatch(1)
            val staleGeneration = resolver.beginGeneration()
            assertEquals(
                Phase0ResolveAdmission.Accepted,
                resolver.resolve(selected.network, "stale.invalid", staleGeneration) {
                    staleCallback.countDown()
                },
            )
            resolver.cancel(staleGeneration)
            assertTrue(!staleCallback.await(250, TimeUnit.MILLISECONDS))
        }
    }

    private fun Phase0NativeTransport.await(
        expected: Phase0WorkerEventType,
        timeoutMilliseconds: Long = 5_000,
    ): Phase0WorkerEvent {
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMilliseconds)
        while (System.nanoTime() < deadline) {
            val remaining = TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime())
                .coerceAtLeast(1)
                .coerceAtMost(250)
                .toInt()
            val event = pollEvent(remaining) ?: continue
            if (event.type == expected) return event
            if (event.type == Phase0WorkerEventType.FAILURE) {
                return event
            }
        }
        throw AssertionError("Timed out waiting for $expected")
    }
}
