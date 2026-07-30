package me.magnum.melonds.impl.network

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import androidx.annotation.RequiresApi
import androidx.core.content.getSystemService
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.Closeable
import java.net.Inet4Address
import javax.inject.Inject
import javax.inject.Singleton

data class Phase0SelectedWifiNetwork(
    val network: Network,
    val networkHandle: Long,
    val interfaceName: String,
    val ipv4Address: ByteArray,
    val ipv4PrefixLength: Int,
    val routes: Set<String>,
) {
    fun hasSameBinding(other: Phase0SelectedWifiNetwork): Boolean =
        networkHandle == other.networkHandle &&
            interfaceName == other.interfaceName &&
            ipv4Address.contentEquals(other.ipv4Address) &&
            ipv4PrefixLength == other.ipv4PrefixLength &&
            routes == other.routes
}

sealed interface Phase0NetworkEvent {
    data class Available(val selected: Phase0SelectedWifiNetwork) : Phase0NetworkEvent
    data object Lost : Phase0NetworkEvent
    data object Changed : Phase0NetworkEvent
    data object Blocked : Phase0NetworkEvent
}

/**
 * Selects an IPv4 Wi-Fi network for Phase 0 without requiring Internet validation.
 *
 * The returned [Network] is retained so hostname resolution and native socket
 * binding use the same Android network instead of the process/default network.
 */
@Singleton
class Phase0WifiNetworkProvider @Inject constructor(
    @ApplicationContext context: Context,
) {
    private val connectivityManager = requireNotNull(
        context.getSystemService<ConnectivityManager>(),
    )

    fun select(): Phase0SelectedWifiNetwork? {
        val active = connectivityManager.activeNetwork
        if (active != null) {
            select(active)?.let { return it }
        }

        return connectivityManager.allNetworks
            .asSequence()
            .mapNotNull(::select)
            .firstOrNull()
    }

    fun observe(
        initial: Phase0SelectedWifiNetwork,
        listener: (Phase0NetworkEvent) -> Unit,
    ): Closeable {
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onLost(network: Network) {
                if (network == initial.network) {
                    listener(Phase0NetworkEvent.Lost)
                }
            }

            override fun onCapabilitiesChanged(
                network: Network,
                networkCapabilities: NetworkCapabilities,
            ) {
                if (network != initial.network) return
                if (!networkCapabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    listener(Phase0NetworkEvent.Changed)
                }
            }

            override fun onLinkPropertiesChanged(
                network: Network,
                linkProperties: LinkProperties,
            ) {
                if (network != initial.network) return
                val updated = select(network, linkProperties)
                when {
                    updated == null -> listener(Phase0NetworkEvent.Lost)
                    !initial.hasSameBinding(updated) -> listener(Phase0NetworkEvent.Changed)
                }
            }

            override fun onBlockedStatusChanged(network: Network, blocked: Boolean) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
                    network == initial.network &&
                    blocked
                ) {
                    listener(Phase0NetworkEvent.Blocked)
                }
            }
        }

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .build()
        connectivityManager.registerNetworkCallback(request, callback)
        return Closeable { connectivityManager.unregisterNetworkCallback(callback) }
    }

    private fun select(network: Network): Phase0SelectedWifiNetwork? {
        val capabilities = connectivityManager.getNetworkCapabilities(network) ?: return null
        if (!capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return null
        val linkProperties = connectivityManager.getLinkProperties(network) ?: return null
        return select(network, linkProperties)
    }

    private fun select(
        network: Network,
        linkProperties: LinkProperties,
    ): Phase0SelectedWifiNetwork? {
        val address = linkProperties.linkAddresses.firstOrNull {
            it.address is Inet4Address
        } ?: return null
        val interfaceName = linkProperties.interfaceName ?: return null
        return Phase0SelectedWifiNetwork(
            network = network,
            networkHandle = network.networkHandle,
            interfaceName = interfaceName,
            ipv4Address = address.address.address.copyOf(),
            ipv4PrefixLength = address.prefixLength,
            routes = linkProperties.routes.mapTo(sortedSetOf()) { it.toString() },
        )
    }
}
