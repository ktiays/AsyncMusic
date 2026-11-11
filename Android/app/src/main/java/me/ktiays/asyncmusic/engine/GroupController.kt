//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import android.content.Context
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
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
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Everything the app runs, for the whole process: the native share center (listening for
 * shares, joining one, or sharing a file), its group session and audio output. While joined or
 * sharing it keeps the process (through [PlaybackService]), the CPU and Wi-Fi awake, since the
 * clock sync and the music would stop otherwise; while the app is open it also keeps Wi-Fi
 * passing broadcasts, which is how shares are heard.
 *
 * State is published on the main thread through [shareState] (and [state], the session's
 * snapshot, for diagnostics); [live] reads what changes continuously and is meant to be polled
 * by screens that show it.
 */
class GroupController(private val context: Context) {
    val settings = Settings(context)
    private val main = Handler(Looper.getMainLooper())
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** What kind of device this is, as listeners picture it. */
    val deviceKind: ShareDeviceKind =
        if (context.resources.configuration.smallestScreenWidthDp >= 600) ShareDeviceKind.Tablet else ShareDeviceKind.Phone

    private val engine = NativeEngine(
        object : NativeEngine.Callbacks {
            override fun onChange() = scheduleRefresh()

            override fun onAlert(share: ByteArray) {
                val parsed = NearbyShare.parse(share.decodeToString())
                main.post { handleAlert(parsed) }
            }

            override fun onSpeakerSettingsChange(layout: Int, left: Long, right: Long) {
                settings.speakers = SpeakerSettings(SpeakerLayout.of(layout), left, right)
            }
        },
        NativeEngine.Options(
            identity = settings.identity,
            deviceKind = deviceKind,
            model = Build.MODEL,
            speakers = settings.speakers,
            manualOffset = settings.manualOffset,
            volume = settings.volume,
            alertsEnabled = settings.alertBehavior == AlertBehavior.ShowAlert,
        ),
    )

    private val _shareState = MutableStateFlow(engine.state())
    val shareState: StateFlow<ShareState> = _shareState.asStateFlow()

    private val _state = MutableStateFlow(engine.snapshot())
    /** The session's snapshot, for diagnostics. */
    val state: StateFlow<Snapshot> = _state.asStateFlow()

    private val _deviceName = MutableStateFlow(settings.identity.name)
    /** The name others see. */
    val deviceName: StateFlow<String> = _deviceName.asStateFlow()

    private val _volume = MutableStateFlow(settings.volume)
    val volume: StateFlow<Float> = _volume.asStateFlow()

    private val _alertBehavior = MutableStateFlow(settings.alertBehavior)
    val alertBehavior: StateFlow<AlertBehavior> = _alertBehavior.asStateFlow()

    /** The audio file chosen to share: still being read, or why it could not be. */
    data class SourceStatus(val isLoading: Boolean = false, val error: String? = null)

    private val _sourceStatus = MutableStateFlow(SourceStatus())
    val sourceStatus: StateFlow<SourceStatus> = _sourceStatus.asStateFlow()

    private val _modeRequest = MutableStateFlow<ShareMode?>(null)
    /** A mode switch asked for outside the UI ("Open with") that needs the user's confirmation. */
    val modeRequest: StateFlow<ShareMode?> = _modeRequest.asStateFlow()

    private val refreshPending = AtomicBoolean(false)
    private var isAppVisible = false
    /** Until when to keep the service up for a join or share that hasn't shown up in the state yet. */
    private var pendingUntil = 0L
    private var loadGeneration = 0
    /** A share to join as soon as it's heard (asked for while it wasn't). */
    private var pendingJoin: Long? = null

    private val wakeLock = context.getSystemService(PowerManager::class.java)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AsyncMusic:group")
        .apply { setReferenceCounted(false) }
    private val wifi = context.getSystemService(WifiManager::class.java)
    private val wifiLock = wifi.let { wifi ->
        // Low latency mode keeps the radio out of power save, which otherwise adds tens of
        // milliseconds of jitter to the clock probes (it applies while the app is in front).
        @Suppress("DEPRECATION")
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifi.createWifiLock(mode, "AsyncMusic:group").apply { setReferenceCounted(false) }
    }
    /** Many devices drop broadcast datagrams (share announcements) unless an app holds this. */
    private val multicastLock = wifi.createMulticastLock("AsyncMusic:shares").apply { setReferenceCounted(false) }

    fun live(): Live = engine.live()

    /** The app came to the front or went away. In front, shares are listed and alerts show in the app. */
    fun setAppVisible(visible: Boolean) {
        isAppVisible = visible
        if (visible) {
            ShareNotifications.cancelAll(context)
            // Like opening the Mac's panel: clears alerts and asks sharers to announce now.
            engine.refresh()
        }
        updateLocks(_shareState.value)
    }

    fun setMode(mode: ShareMode) {
        _modeRequest.value = null
        engine.setMode(mode)
    }

    fun cancelModeRequest() {
        _modeRequest.value = null
    }

    fun join(share: Long) {
        holdService()
        ShareNotifications.cancel(context, share)
        // From a notification, the share may have dropped out of the list while Android kept the
        // app off the network in the background; the service lifts that, the sharer is heard
        // again within a couple of seconds, and the join goes through then (see `refresh`).
        pendingJoin = share
        engine.join(share)
        if (_shareState.value.nearby.none { it.id == share }) engine.refresh()
    }

    /**
     * Follows the leader at "host:port" (IPv6 in brackets), for networks where shares can't be
     * heard (an emulator behind NAT). Returns why it can't, or null.
     */
    fun joinAddress(text: String): String? {
        val trimmed = text.trim()
        val colon = trimmed.lastIndexOf(':')
        if (colon <= 0) return "Enter an address and port, like 192.168.1.20:50000."
        val host = trimmed.substring(0, colon).removePrefix("[").removeSuffix("]")
        val port = trimmed.substring(colon + 1).toIntOrNull()
        if (port == null || port !in 1..65535) return "The port must be a number from 1 to 65535."
        holdService()
        if (!engine.joinAddress(trimmed, host, port)) {
            pendingUntil = 0
            updateService(_shareState.value)
            return "“$host” isn’t a numeric IP address."
        }
        return null
    }

    fun leave() = engine.leave()

    private val _isPausedHere = MutableStateFlow(false)
    /** Listening, but silent on this device; the stream and the sync go on. */
    val isPausedHere: StateFlow<Boolean> = _isPausedHere.asStateFlow()

    fun setPausedHere(paused: Boolean) {
        _isPausedHere.value = paused
        engine.setPausedHere(paused)
    }

    /** Sharing: plays the file from the top for everyone. (Core has no pause or seek yet.) */
    fun playForEveryone() = engine.playSelected()
    fun stopForEveryone() = engine.stopPlayback()
    fun requestChannel(channel: SpeakerChannel) = engine.requestChannel(channel)
    fun dismissAlerts() = engine.dismissAlerts()

    fun startSharing() {
        holdService()
        engine.startSharing()
    }

    fun stopSharing() = engine.stopSharing()
    fun setLayout(layout: SpeakerLayout) = engine.setLayout(layout)
    fun clearLog() = engine.clearLog()

    fun setVolume(volume: Float) {
        val clamped = volume.coerceIn(0f, 1f)
        _volume.value = clamped
        engine.setVolume(clamped)
        settings.volume = clamped
    }

    fun setAlertBehavior(behavior: AlertBehavior) {
        settings.alertBehavior = behavior
        _alertBehavior.value = behavior
        engine.setAlertsEnabled(behavior == AlertBehavior.ShowAlert)
    }

    var manualOffset: Double
        get() = settings.manualOffset
        set(value) {
            settings.manualOffset = value
            engine.setManualOffset(value)
        }

    /** Pass an empty name to go back to the device's own. */
    fun rename(name: String) {
        settings.customDeviceName = name
        val identity = settings.identity
        _deviceName.value = identity.name
        engine.setIdentity(identity)
        scheduleRefresh()
    }

    /**
     * Chooses the audio file at [uri] to share: names it right away, then copies it into the
     * app's storage (hashing it on the way, for its ID) and loads it into the session.
     */
    fun chooseFile(uri: Uri) {
        if (_shareState.value.isSharing) return
        val generation = ++loadGeneration
        _sourceStatus.value = SourceStatus(isLoading = true)
        engine.setSourceStatus(true, null)
        scope.launch {
            val displayName = withContext(Dispatchers.IO) { runCatching { displayName(uri) }.getOrNull() } ?: uri.lastPathSegment ?: "Audio"
            if (generation != loadGeneration) return@launch
            engine.selectSource(ShareSource(displayName.substringBeforeLast('.').ifEmpty { displayName }))
            val error = withContext(Dispatchers.IO) {
                runCatching { importTrack(uri, displayName) }.getOrElse { it.message ?: "Couldn’t read the file." }
            }
            if (generation != loadGeneration) return@launch
            engine.setSourceStatus(false, error)
            _sourceStatus.value = SourceStatus(error = error)
            scheduleRefresh()
        }
    }

    /** "Open with": share that file, after asking if that would end a share being listened to. */
    fun openFile(uri: Uri) {
        val current = _shareState.value
        if (current.isSharing) return
        chooseFile(uri)
        if (current.switchInterrupts(ShareMode.Share)) {
            _modeRequest.value = ShareMode.Share
        } else {
            engine.setMode(ShareMode.Share)
        }
    }

    private fun displayName(uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }

    private fun importTrack(uri: Uri, displayName: String): String? {
        val resolver = context.contentResolver
        val directory = File(context.noBackupFilesDir, "tracks").apply { mkdirs() }
        val partial = File(directory, "import.partial")
        val digest = MessageDigest.getInstance("SHA-256")
        val input = resolver.openInputStream(uri) ?: return "Couldn’t open the file."
        try {
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
        } catch (error: Exception) {
            // Don't leave a partial copy behind (a full disk is the usual reason).
            partial.delete()
            throw error
        }
        val hash = digest.digest()
        val extension = displayName.substringAfterLast('.', "").takeIf { it.isNotEmpty() && it.length <= 5 }?.let { ".$it" } ?: ""
        val file = File(directory, hash.joinToString("") { "%02x".format(it) }.take(16) + extension)
        if (!partial.renameTo(file)) return "Couldn’t store the file."
        // Keep the few most recent; streams of older ones may still be reading theirs.
        directory.listFiles { candidate -> candidate != file && candidate.name != partial.name }
            ?.sortedByDescending { it.lastModified() }
            ?.drop(2)
            ?.forEach { it.delete() }
        return engine.loadTrack(file.path, displayName.substringBeforeLast('.').ifEmpty { displayName }, hash)
    }

    /** A new share while alerts are on: in the app it shows as an alert; elsewhere as a notification. */
    private fun handleAlert(share: NearbyShare) {
        if (isAppVisible) return
        ShareNotifications.post(context, share)
        // The notification stands for the alert; in the app the share is just listed (unseen).
        engine.dismissAlerts()
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
        val shareState = engine.state()
        val previousJoined = _shareState.value.joined?.id
        _shareState.value = shareState
        _state.value = engine.snapshot()
        // A new share (or none) starts out audible.
        if (_isPausedHere.value && (shareState.joined == null || shareState.joined.id != previousJoined)) setPausedHere(false)
        pendingJoin?.let { share ->
            when {
                shareState.joined?.id == share || SystemClock.elapsedRealtime() >= pendingUntil -> pendingJoin = null
                shareState.nearby.any { it.id == share } -> engine.join(share)
            }
        }
        if (shareState.isActive) pendingUntil = 0
        // Notifications of shares that ended (or were joined) go away. Only while shares can be
        // heard: in the background, outside a group, Android keeps the app off the network, so
        // a share dropping out of the list says nothing about whether it ended.
        if (isAppVisible || shareState.isActive) {
            ShareNotifications.retain(context, shareState.nearbyOthers.map { it.id }.toSet())
        }
        updateService(shareState)
        updateLocks(shareState)
    }

    /** Starts the service and the locks for a join or share the state doesn't show yet. */
    private fun holdService() {
        pendingUntil = SystemClock.elapsedRealtime() + PENDING_TIMEOUT
        main.postDelayed({ updateService(_shareState.value) }, PENDING_TIMEOUT + 50)
        updateService(_shareState.value)
    }

    private fun updateService(state: ShareState) {
        val pending = SystemClock.elapsedRealtime() < pendingUntil
        val active = state.isActive || pending
        stayAwake(active)
        if (active) {
            PlaybackService.start(context)
            PlaybackService.update(context, state, pending && !state.isActive)
        } else {
            PlaybackService.stop(context)
        }
    }

    private fun updateLocks(state: ShareState) {
        val listens = state.joined != null || (isAppVisible && state.mode == ShareMode.Listen)
        if (listens) {
            multicastLock.acquire()
        } else if (multicastLock.isHeld) {
            multicastLock.release()
        }
    }

    private fun stayAwake(awake: Boolean) {
        if (awake) {
            if (!wakeLock.isHeld) wakeLock.acquire()
            if (!wifiLock.isHeld) wifiLock.acquire()
        } else {
            if (wakeLock.isHeld) wakeLock.release()
            if (wifiLock.isHeld) wifiLock.release()
        }
    }

    companion object {
        private const val PENDING_TIMEOUT = 8_000L

        @Volatile private var shared: GroupController? = null

        fun get(context: Context): GroupController =
            shared ?: synchronized(this) {
                shared ?: GroupController(context.applicationContext).also { shared = it }
            }
    }
}
