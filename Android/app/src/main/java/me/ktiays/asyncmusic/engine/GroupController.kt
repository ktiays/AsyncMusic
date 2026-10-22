//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import android.content.Context
import android.net.Uri
import android.net.nsd.NsdManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.OpenableColumns
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Everything the app runs, for the whole process: the native group session and its audio
 * output, the search for leaders and, when hosting, this device's advertisement, kept in step
 * with the role. While part of a group it keeps the process (through [PlaybackService]), the
 * CPU and Wi-Fi awake, since the clock sync and the music would stop otherwise.
 *
 * State is published on the main thread through [state]; [live] reads what changes
 * continuously and is meant to be polled by screens that show it.
 */
class GroupController(private val context: Context) {
    val settings = Settings(context)
    private val nsd = context.getSystemService(NsdManager::class.java)
    val browser = LeaderBrowser(nsd)
    private val advertiser = LeaderAdvertiser(nsd)
    private val main = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** Leaders joined by address rather than found by mDNS: name to "host port". */
    private val manualLeaders = ConcurrentHashMap<String, String>()

    private val engine = NativeEngine(
        object : NativeEngine.Callbacks {
            override fun onChange() = scheduleRefresh()

            override fun onSpeakerSettingsChange(layout: Int, left: Long, right: Long) {
                settings.speakers = SpeakerSettings(SpeakerLayout.of(layout), left, right)
            }

            override fun resolve(name: ByteArray): String? {
                val leader = name.decodeToString()
                return manualLeaders[leader] ?: browser.resolve(leader)
            }
        },
        settings.identity,
        settings.speakers,
        settings.manualOffset,
    )

    private val _state = MutableStateFlow(engine.snapshot())
    val state: StateFlow<Snapshot> = _state.asStateFlow()

    private val _role = MutableStateFlow(Role.Off)
    /** The role asked for last; the snapshot's follows once the session has applied it. */
    val role: StateFlow<Role> = _role.asStateFlow()

    private val _isLoadingTrack = MutableStateFlow(false)
    val isLoadingTrack: StateFlow<Boolean> = _isLoadingTrack.asStateFlow()

    private val _loadError = MutableStateFlow<String?>(null)
    /** Why the last file could not be loaded. */
    val loadError: StateFlow<String?> = _loadError.asStateFlow()

    /** Why this device cannot be found by others while hosting. */
    val advertisingError: StateFlow<String?> get() = advertiser.error

    private val refreshPending = AtomicBoolean(false)

    private val wakeLock = context.getSystemService(PowerManager::class.java)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AsyncMusic:group")
        .apply { setReferenceCounted(false) }
    private val wifiLock = context.getSystemService(WifiManager::class.java).let { wifi ->
        // Low latency mode keeps the radio out of power save, which otherwise adds tens of
        // milliseconds of jitter to the clock probes (it applies while the app is in front).
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifi.createWifiLock(mode, "AsyncMusic:group").apply { setReferenceCounted(false) }
    }

    fun live(): Live = engine.live()

    fun setRole(role: Role) {
        _role.value = role
        engine.setRole(role)
        if (role == Role.Follower) browser.start() else browser.stop()
        stayAwake(role != Role.Off)
        if (role == Role.Off) {
            PlaybackService.stop(context)
        } else {
            PlaybackService.start(context)
        }
        scheduleRefresh()
    }

    fun follow(leader: String) {
        manualLeaders.remove(leader)
        engine.follow(leader)
        scheduleRefresh()
    }

    /** Follows the leader at [host] (a numeric address) and [port], for networks without mDNS. */
    fun follow(host: String, port: Int) {
        val name = if (':' in host) "[$host]:$port" else "$host:$port"
        manualLeaders[name] = "$host $port"
        engine.follow(name)
        scheduleRefresh()
    }

    fun play(source: Long) = engine.play(source)
    fun stopPlayback() = engine.stop()
    fun setLayout(layout: SpeakerLayout) = engine.setLayout(layout)
    fun assign(device: Long, channel: SpeakerChannel) = engine.assign(device, channel)
    fun swapChannels() = engine.swapChannels()
    fun clearLog() = engine.clearLog()

    var manualOffset: Double
        get() = settings.manualOffset
        set(value) {
            settings.manualOffset = value
            engine.setManualOffset(value)
        }

    /** Pass an empty name to go back to the device's own. Takes effect from the next join. */
    fun rename(name: String) {
        settings.customDeviceName = name
        engine.setIdentity(settings.identity)
        scheduleRefresh()
    }

    /**
     * Copies the file at [uri] into the app's storage (hashing it on the way, for its ID) and,
     * once read, selects it for playing.
     */
    fun loadTrack(uri: Uri) {
        _isLoadingTrack.value = true
        _loadError.value = null
        scope.launch {
            val error = withContext(Dispatchers.IO) {
                runCatching { importTrack(uri) }.getOrElse { it.message ?: "Could not read the file" }
            }
            _loadError.value = error
            _isLoadingTrack.value = false
            scheduleRefresh()
        }
    }

    private fun importTrack(uri: Uri): String? {
        val resolver = context.contentResolver
        val displayName = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        } ?: uri.lastPathSegment ?: "Music"
        val directory = File(context.noBackupFilesDir, "tracks").apply { mkdirs() }
        val partial = File(directory, "import.partial")
        val digest = MessageDigest.getInstance("SHA-256")
        val input = resolver.openInputStream(uri) ?: return "Could not open the file"
        input.use { stream ->
            partial.outputStream().use { output ->
                val buffer = ByteArray(1 shl 16)
                while (true) {
                    val count = stream.read(buffer)
                    if (count < 0) break
                    digest.update(buffer, 0, count)
                    output.write(buffer, 0, count)
                }
            }
        }
        val hash = digest.digest()
        val extension = displayName.substringAfterLast('.', "").takeIf { it.isNotEmpty() && it.length <= 5 }?.let { ".$it" } ?: ""
        val file = File(directory, hash.joinToString("") { "%02x".format(it) }.take(16) + extension)
        if (!partial.renameTo(file)) return "Could not store the file"
        // Keep the few most recent; streams of older ones may still be reading theirs.
        directory.listFiles { candidate -> candidate != file && candidate.name != partial.name }
            ?.sortedByDescending { it.lastModified() }
            ?.drop(2)
            ?.forEach { it.delete() }
        return engine.loadTrack(file.path, displayName.substringBeforeLast('.').ifEmpty { displayName }, hash)
    }

    /** Coalesces the engine's change notifications into one refresh on the main thread. */
    private fun scheduleRefresh() {
        if (!refreshPending.compareAndSet(false, true)) return
        main.post {
            refreshPending.set(false)
            refresh()
        }
    }

    private fun refresh() {
        val previous = _state.value
        val snapshot = engine.snapshot()
        _state.value = snapshot
        // The session left its group by itself. (Only on that change: a role just requested is
        // still off here until the session has applied it.)
        if (snapshot.role == Role.Off && previous.role != Role.Off && _role.value != Role.Off) {
            setRole(Role.Off)
            return
        }
        // Advertise the session's clock port under this device's name while leading.
        if (snapshot.role == Role.Leader && snapshot.leaderPort != 0) {
            advertiser.advertise(snapshot.identity.name, snapshot.leaderPort)
        } else {
            advertiser.stop()
        }
        PlaybackService.update(context, snapshot)
    }

    private fun stayAwake(awake: Boolean) {
        if (awake) {
            wakeLock.acquire()
            wifiLock.acquire()
        } else {
            if (wakeLock.isHeld) wakeLock.release()
            if (wifiLock.isHeld) wifiLock.release()
        }
    }

    companion object {
        @Volatile private var shared: GroupController? = null

        fun get(context: Context): GroupController =
            shared ?: synchronized(this) {
                shared ?: GroupController(context.applicationContext).also { shared = it }
            }
    }
}
