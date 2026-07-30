package me.magnum.melonds.impl.network

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import androidx.core.content.getSystemService
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.Closeable
import javax.inject.Inject

enum class Phase0LowLatencyResult {
    ACQUIRED,
    NOT_FOREGROUND_OR_SCREEN_ON,
    UNSUPPORTED_API,
    WIFI_SERVICE_UNAVAILABLE,
    ACQUISITION_FAILED,
}

/**
 * Lifecycle-owned, best-effort API-29+ low-latency Wi-Fi experiment.
 *
 * The caller supplies foreground/screen state for each measurement run and
 * must release after success, failure, teardown, or leaving that state.
 */
class Phase0LowLatencyWifiController @Inject constructor(
    @ApplicationContext private val context: Context,
) : Closeable {
    private var lock: WifiManager.WifiLock? = null

    @Synchronized
    fun acquireForMeasurement(
        applicationForeground: Boolean,
        screenOn: Boolean,
    ): Phase0LowLatencyResult {
        release()
        if (!applicationForeground || !screenOn) {
            return Phase0LowLatencyResult.NOT_FOREGROUND_OR_SCREEN_ON
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return Phase0LowLatencyResult.UNSUPPORTED_API
        }
        val wifiManager = context.getSystemService<WifiManager>()
            ?: return Phase0LowLatencyResult.WIFI_SERVICE_UNAVAILABLE
        return try {
            lock = wifiManager.createWifiLock(
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY,
                "melonDS:Phase0MultiplayerLatency",
            ).apply {
                setReferenceCounted(false)
                acquire()
            }
            Phase0LowLatencyResult.ACQUIRED
        } catch (_: RuntimeException) {
            lock = null
            Phase0LowLatencyResult.ACQUISITION_FAILED
        }
    }

    @Synchronized
    fun release() {
        val current = lock
        lock = null
        if (current?.isHeld == true) {
            try {
                current.release()
            } catch (_: RuntimeException) {
                // Best-effort experiment cleanup; transport is independent.
            }
        }
    }

    override fun close() = release()
}
