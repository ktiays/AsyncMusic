//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

/**
 * The native engine (`Engine.cpp`): Core's group session and the AAudio output playing it.
 *
 * State comes back as JSON (see [Snapshot] and [Live]); strings cross the boundary as UTF-8
 * bytes, since JNI's modified UTF-8 mangles characters outside the BMP.
 */
class NativeEngine(
    callbacks: Callbacks,
    identity: DeviceIdentity,
    speakers: SpeakerSettings,
    manualOffset: Double,
) {
    /** Called from the engine's own threads. */
    interface Callbacks {
        /** The session changed. */
        fun onChange()

        /** The leader's speaker layout or assignments changed; keep them for next time. */
        fun onSpeakerSettingsChange(layout: Int, left: Long, right: Long)

        /**
         * Looks up where the leader called [name] (UTF-8) listens now, as "host port". Blocks for
         * a few seconds at most.
         */
        fun resolve(name: ByteArray): String?
    }

    private var handle: Long = nativeCreate(
        callbacks,
        identity.id,
        identity.name.encodeToByteArray(),
        speakers.layout.raw,
        speakers.left,
        speakers.right,
        manualOffset,
    )

    fun close() {
        if (handle != 0L) nativeDestroy(handle)
        handle = 0
    }

    fun setRole(role: Role) = nativeSetRole(handle, role.ordinal)
    fun setIdentity(identity: DeviceIdentity) = nativeSetIdentity(handle, identity.id, identity.name.encodeToByteArray())
    fun follow(leader: String) = nativeFollow(handle, leader.encodeToByteArray())

    /** Blocks while the file's start is decoded; returns why it could not be loaded, or null. */
    fun loadTrack(path: String, name: String, digest: ByteArray): String? =
        nativeLoadTrack(handle, path, name.encodeToByteArray(), digest)?.decodeToString()

    fun play(source: Long) = nativePlay(handle, source)
    fun stop() = nativeStop(handle)
    fun setLayout(layout: SpeakerLayout) = nativeSetLayout(handle, layout.raw)

    /** Plays [channel] on [device] (0: nobody). */
    fun assign(device: Long, channel: SpeakerChannel) = nativeAssign(handle, device, channel.raw)
    fun swapChannels() = nativeSwapChannels(handle)
    fun clearLog() = nativeClearLog(handle)
    fun setManualOffset(milliseconds: Double) = nativeSetManualOffset(handle, milliseconds)

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
            layout: Int,
            left: Long,
            right: Long,
            manualOffset: Double,
        ): Long

        @JvmStatic external fun nativeDestroy(handle: Long)
        @JvmStatic external fun nativeSetRole(handle: Long, role: Int)
        @JvmStatic external fun nativeSetIdentity(handle: Long, id: Long, name: ByteArray)
        @JvmStatic external fun nativeFollow(handle: Long, leader: ByteArray)
        @JvmStatic external fun nativeLoadTrack(handle: Long, path: String, name: ByteArray, digest: ByteArray): ByteArray?
        @JvmStatic external fun nativePlay(handle: Long, source: Long)
        @JvmStatic external fun nativeStop(handle: Long)
        @JvmStatic external fun nativeSetLayout(handle: Long, layout: Int)
        @JvmStatic external fun nativeAssign(handle: Long, device: Long, channel: Int)
        @JvmStatic external fun nativeSwapChannels(handle: Long)
        @JvmStatic external fun nativeClearLog(handle: Long)
        @JvmStatic external fun nativeSetManualOffset(handle: Long, milliseconds: Double)
        @JvmStatic external fun nativeSnapshot(handle: Long): ByteArray
        @JvmStatic external fun nativeLive(handle: Long): ByteArray
    }
}
