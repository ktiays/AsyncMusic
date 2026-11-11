//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

/**
 * The native engine (`Engine.cpp`): Core's group session, the AAudio output playing it and the
 * share center that listens for shares, joins them and shares a file.
 *
 * State comes back as JSON (see [ShareState], [Snapshot] and [Live]); strings cross the boundary
 * as UTF-8 bytes, since JNI's modified UTF-8 mangles characters outside the BMP.
 */
class NativeEngine(callbacks: Callbacks, options: Options) {
    /** Called from the engine's own threads. */
    interface Callbacks {
        /** The share center or the session changed. */
        fun onChange()

        /** A new share arrived and alerts are on: [share] is its JSON (see [NearbyShare]). */
        fun onAlert(share: ByteArray)

        /** The leader's speaker layout or assignments changed; keep them for next time. */
        fun onSpeakerSettingsChange(layout: Int, left: Long, right: Long)
    }

    class Options(
        val identity: DeviceIdentity,
        val deviceKind: ShareDeviceKind,
        /** E.g. "Pixel 10 Pro", which listeners show. */
        val model: String,
        val speakers: SpeakerSettings,
        val manualOffset: Double,
        val volume: Float,
        val alertsEnabled: Boolean,
    )

    private var handle: Long = nativeCreate(
        callbacks,
        options.identity.id,
        options.identity.name.encodeToByteArray(),
        options.deviceKind.raw,
        options.model.encodeToByteArray(),
        options.speakers.layout.raw,
        options.speakers.left,
        options.speakers.right,
        options.manualOffset,
        options.volume,
        options.alertsEnabled,
    )

    fun close() {
        if (handle != 0L) nativeDestroy(handle)
        handle = 0
    }

    fun setIdentity(identity: DeviceIdentity) = nativeSetIdentity(handle, identity.id, identity.name.encodeToByteArray())

    // Share center
    fun setMode(mode: ShareMode) = nativeSetMode(handle, mode == ShareMode.Share)
    fun setAlertsEnabled(enabled: Boolean) = nativeSetAlertsEnabled(handle, enabled)
    fun join(share: Long) = nativeJoin(handle, share)

    /** Follows the leader at [host] (numeric) and [port]; false if that isn't an address. */
    fun joinAddress(name: String, host: String, port: Int): Boolean = nativeJoinAddress(handle, name.encodeToByteArray(), host, port)
    fun leave() = nativeLeave(handle)

    /** In a stereo pair: asks to play [channel] (left or right) here. */
    fun requestChannel(channel: SpeakerChannel) = nativeRequestChannel(handle, channel.raw)
    fun dismissAlerts() = nativeDismissAlerts(handle)
    fun refresh() = nativeRefresh(handle)
    fun selectSource(source: ShareSource) = nativeSelectSource(handle, source.name.encodeToByteArray(), source.kind.raw)
    fun startSharing() = nativeStartSharing(handle)
    fun stopSharing() = nativeStopSharing(handle)
    fun setLayout(layout: SpeakerLayout) = nativeSetLayout(handle, layout.raw)

    /** Blocks while the file's start is decoded; returns why it could not be loaded, or null. */
    fun loadTrack(path: String, name: String, digest: ByteArray): String? =
        nativeLoadTrack(handle, path, name.encodeToByteArray(), digest)?.decodeToString()

    /** Tells the share center the source is loading, or why it could not be loaded. */
    fun setSourceStatus(isLoading: Boolean, error: String?) = nativeSetSourceStatus(handle, isLoading, error.orEmpty().encodeToByteArray())

    fun clearLog() = nativeClearLog(handle)
    fun setManualOffset(milliseconds: Double) = nativeSetManualOffset(handle, milliseconds)
    fun setVolume(volume: Float) = nativeSetVolume(handle, volume)
    fun setPausedHere(paused: Boolean) = nativeSetPausedHere(handle, paused)
    fun playSelected() = nativePlaySelected(handle)
    fun stopPlayback() = nativeStopPlayback(handle)

    fun state(): ShareState = ShareState.parse(nativeState(handle).decodeToString())
    fun snapshot(): Snapshot = Snapshot.parse(nativeSnapshot(handle).decodeToString())
    fun live(): Live = Live.parse(nativeLive(handle).decodeToString())

    private companion object {
        init {
            System.loadLibrary("asyncmusic")
        }

        @JvmStatic external fun nativeCreate(
            callbacks: Callbacks,
            id: Long,
            name: ByteArray,
            deviceKind: Int,
            model: ByteArray,
            layout: Int,
            left: Long,
            right: Long,
            manualOffset: Double,
            volume: Float,
            alertsEnabled: Boolean,
        ): Long

        @JvmStatic external fun nativeDestroy(handle: Long)
        @JvmStatic external fun nativeSetIdentity(handle: Long, id: Long, name: ByteArray)
        @JvmStatic external fun nativeSetMode(handle: Long, share: Boolean)
        @JvmStatic external fun nativeSetAlertsEnabled(handle: Long, enabled: Boolean)
        @JvmStatic external fun nativeJoin(handle: Long, share: Long)
        @JvmStatic external fun nativeJoinAddress(handle: Long, name: ByteArray, host: String, port: Int): Boolean
        @JvmStatic external fun nativeLeave(handle: Long)
        @JvmStatic external fun nativeRequestChannel(handle: Long, channel: Int)
        @JvmStatic external fun nativeDismissAlerts(handle: Long)
        @JvmStatic external fun nativeRefresh(handle: Long)
        @JvmStatic external fun nativeSelectSource(handle: Long, name: ByteArray, kind: Int)
        @JvmStatic external fun nativeStartSharing(handle: Long)
        @JvmStatic external fun nativeStopSharing(handle: Long)
        @JvmStatic external fun nativeSetLayout(handle: Long, layout: Int)
        @JvmStatic external fun nativeLoadTrack(handle: Long, path: String, name: ByteArray, digest: ByteArray): ByteArray?
        @JvmStatic external fun nativeSetSourceStatus(handle: Long, isLoading: Boolean, error: ByteArray)
        @JvmStatic external fun nativeClearLog(handle: Long)
        @JvmStatic external fun nativeSetManualOffset(handle: Long, milliseconds: Double)
        @JvmStatic external fun nativeSetVolume(handle: Long, volume: Float)
        @JvmStatic external fun nativeSetPausedHere(handle: Long, paused: Boolean)
        @JvmStatic external fun nativePlaySelected(handle: Long)
        @JvmStatic external fun nativeStopPlayback(handle: Long)
        @JvmStatic external fun nativeState(handle: Long): ByteArray
        @JvmStatic external fun nativeSnapshot(handle: Long): ByteArray
        @JvmStatic external fun nativeLive(handle: Long): ByteArray
    }
}
