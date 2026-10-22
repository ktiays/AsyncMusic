//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.Inet4Address
import java.net.InetAddress
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** The mDNS service type leaders advertise their clock under (Core's `kClockSyncServiceType`). */
const val CLOCK_SYNC_SERVICE_TYPE = "_asyncmusic._udp"

/**
 * Finds leaders on the local network through the system's mDNS (`NsdManager`): instances of
 * [CLOCK_SYNC_SERVICE_TYPE], which leaders register under their device name.
 */
class LeaderBrowser(private val nsd: NsdManager) {
    data class Leader(val name: String)

    private val _leaders = MutableStateFlow<List<Leader>>(emptyList())
    /** Current leaders, sorted by name. */
    val leaders: StateFlow<List<Leader>> = _leaders.asStateFlow()

    private val _error = MutableStateFlow<String?>(null)
    val error: StateFlow<String?> = _error.asStateFlow()

    private val lock = Any()
    private var listener: NsdManager.DiscoveryListener? = null
    private val found = sortedMapOf<String, Leader>(String.CASE_INSENSITIVE_ORDER)

    val isBrowsing: Boolean get() = synchronized(lock) { listener != null }

    fun start() = synchronized(lock) {
        if (listener != null) return@synchronized
        found.clear()
        publish()
        val discovery = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {
                _error.value = null
            }

            override fun onDiscoveryStopped(serviceType: String) {}

            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.w(TAG, "Discovery failed: $errorCode")
                synchronized(lock) { if (listener === this) listener = null }
                _error.value = "Could not look for hosts (error $errorCode)"
            }

            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}

            override fun onServiceFound(service: NsdServiceInfo) {
                synchronized(lock) {
                    if (listener !== this) return
                    found[service.serviceName] = Leader(service.serviceName)
                    publish()
                }
            }

            override fun onServiceLost(service: NsdServiceInfo) {
                synchronized(lock) {
                    if (listener !== this) return
                    found.remove(service.serviceName)
                    publish()
                }
            }
        }
        listener = discovery
        nsd.discoverServices(CLOCK_SYNC_SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discovery)
    }

    fun stop() {
        synchronized(lock) {
            val discovery = listener ?: return
            listener = null
            found.clear()
            publish()
            runCatching { nsd.stopServiceDiscovery(discovery) }
        }
    }

    private fun publish() {
        _leaders.value = found.values.toList()
    }

    /**
     * Looks up where the leader called [name] listens now, as "host port" (IPv4 preferred: some
     * networks route a host's global IPv6 address nowhere). Blocks for up to [timeoutMillis].
     */
    fun resolve(name: String, timeoutMillis: Long = 3000): String? {
        val deadline = System.nanoTime() + timeoutMillis * 1_000_000
        // Before Android 14 the system resolves one service at a time.
        synchronized(resolveLock) {
            while (System.nanoTime() < deadline) {
                val service = NsdServiceInfo().apply {
                    serviceName = name
                    serviceType = CLOCK_SYNC_SERVICE_TYPE
                }
                var result: String? = null
                var busy = false
                val done = CountDownLatch(1)
                @Suppress("DEPRECATION")
                nsd.resolveService(service, object : NsdManager.ResolveListener {
                    override fun onResolveFailed(service: NsdServiceInfo, errorCode: Int) {
                        busy = errorCode == NsdManager.FAILURE_ALREADY_ACTIVE
                        done.countDown()
                    }

                    override fun onServiceResolved(service: NsdServiceInfo) {
                        val address = addresses(service).sortedBy { if (it is Inet4Address) 0 else 1 }.firstOrNull()
                        if (address != null && service.port > 0) result = "${address.hostAddress} ${service.port}"
                        done.countDown()
                    }
                })
                val remaining = (deadline - System.nanoTime()) / 1_000_000
                if (!done.await(remaining.coerceAtLeast(1), TimeUnit.MILLISECONDS)) return null
                if (result != null) return result
                if (!busy) return null
                Thread.sleep(200)
            }
            return null
        }
    }

    private fun addresses(service: NsdServiceInfo): List<InetAddress> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            service.hostAddresses
        } else {
            @Suppress("DEPRECATION")
            listOfNotNull(service.host)
        }

    private companion object {
        const val TAG = "LeaderBrowser"
        val resolveLock = Any()
    }
}

/**
 * Advertises this device as a leader through the system's mDNS: the instance [name] of
 * [CLOCK_SYNC_SERVICE_TYPE] on the clock sync port.
 */
class LeaderAdvertiser(private val nsd: NsdManager) {
    private val lock = Any()
    private var listener: NsdManager.RegistrationListener? = null
    private var name: String? = null
    private var port = 0

    private val _error = MutableStateFlow<String?>(null)
    /** Why this device cannot be found by others, if it cannot. */
    val error: StateFlow<String?> = _error.asStateFlow()

    /** The name the system registered, which differs from the requested one after a conflict. */
    @Volatile var registeredName: String? = null
        private set

    /** Registers (or re-registers, if the name or port changed) the service. */
    fun advertise(name: String, port: Int) = synchronized(lock) {
        if (listener != null && this.name == name && this.port == port) return@synchronized
        withdraw()
        this.name = name
        this.port = port
        val registration = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(service: NsdServiceInfo) {
                registeredName = service.serviceName
                _error.value = null
            }

            override fun onRegistrationFailed(service: NsdServiceInfo, errorCode: Int) {
                _error.value = "Could not advertise this device (error $errorCode)"
            }

            override fun onServiceUnregistered(service: NsdServiceInfo) {}
            override fun onUnregistrationFailed(service: NsdServiceInfo, errorCode: Int) {}
        }
        listener = registration
        val service = NsdServiceInfo().apply {
            serviceName = name
            serviceType = CLOCK_SYNC_SERVICE_TYPE
            setPort(port)
        }
        nsd.registerService(service, NsdManager.PROTOCOL_DNS_SD, registration)
    }

    fun stop() = synchronized(lock) {
        withdraw()
        _error.value = null
    }

    private fun withdraw() {
        val registration = listener ?: return
        listener = null
        name = null
        port = 0
        registeredName = null
        runCatching { nsd.unregisterService(registration) }
    }
}
