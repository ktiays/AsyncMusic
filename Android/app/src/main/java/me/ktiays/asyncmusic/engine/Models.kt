//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import org.json.JSONArray
import org.json.JSONObject

/** Matches `GroupSession::Role`. */
enum class Role { Off, Leader, Follower }

/** Matches `asyncmusic::SpeakerLayout`. */
enum class SpeakerLayout(val raw: Int) {
    Everywhere(0),
    StereoPair(1);

    companion object {
        fun of(raw: Int) = entries.firstOrNull { it.raw == raw } ?: Everywhere
    }
}

/** Matches `asyncmusic::SpeakerChannel`. */
enum class SpeakerChannel(val raw: Int) {
    Full(0),
    Left(1),
    Right(2);

    companion object {
        fun of(raw: Int) = entries.firstOrNull { it.raw == raw } ?: Full
    }
}

/** Matches Core's `kPlaybackSource*`. */
object PlaybackSource {
    const val NONE = 0L
    const val METRONOME = 1L
    const val CHANNEL_TEST = 2L
}

data class DeviceIdentity(val id: Long, val name: String)

/** The leader's speaker layout and the device chosen for each side (0: nobody). */
data class SpeakerSettings(val layout: SpeakerLayout = SpeakerLayout.Everywhere, val left: Long = 0, val right: Long = 0)

data class Member(val id: Long, val name: String, val isActive: Boolean)

data class Track(val id: Long, val name: String, val duration: Double)

data class Round(
    val index: Int,
    val verdict: String,
    val isAccepted: Boolean,
    val probesSent: Int,
    val probesReceived: Int,
    /** Nanoseconds. */
    val bestRoundTrip: Long?,
    val residual: Long?,
    val offset: Long?,
    val skewPPM: Double?,
    val minimumRoundTrip: Long,
    val residualRMS: Double,
    val acceptedCount: Int,
    val rejectedCount: Int,
    val windowCount: Int,
)

data class LogLine(val date: Long, val text: String)

/** `GroupSession::Snapshot`, as `Engine::snapshotJSON` writes it. */
data class Snapshot(
    val role: Role = Role.Off,
    val status: String = "Idle",
    val identity: DeviceIdentity = DeviceIdentity(0, ""),
    val revision: Long = 0,
    // Follower
    val connectedLeader: String? = null,
    /** connecting, syncing, reconnecting, failed or stopped. */
    val followerState: String? = null,
    val isAudioConnected: Boolean = false,
    val remoteLayout: SpeakerLayout = SpeakerLayout.Everywhere,
    val rounds: List<Round> = emptyList(),
    val latestRound: Round? = null,
    // Leader
    val leaderPort: Int = 0,
    val followerCount: Int = 0,
    val members: List<Member> = emptyList(),
    val layout: SpeakerLayout = SpeakerLayout.Everywhere,
    val assignments: Map<SpeakerChannel, Long> = emptyMap(),
    val activeChannels: Map<Long, SpeakerChannel> = emptyMap(),
    val isLayoutComplete: Boolean = false,
    val isPreparingPlayback: Boolean = false,
    val selectedTrack: Track? = null,
    // Playback
    val source: Long = PlaybackSource.NONE,
    val isPlaying: Boolean = false,
    val sourceDescription: String = "nothing",
    val channel: SpeakerChannel = SpeakerChannel.Full,
    val hasClockMap: Boolean = false,
    val isWaitingForAudio: Boolean = false,
    val log: List<LogLine> = emptyList(),
) {
    /** This device, or a member that is reachable now. */
    fun isDevicePresent(id: Long) = id == identity.id || members.any { it.id == id && it.isActive }

    fun nameOfDevice(id: Long): String? = if (id == identity.id) identity.name else members.firstOrNull { it.id == id }?.name

    companion object {
        fun parse(text: String): Snapshot {
            val json = JSONObject(text)
            return Snapshot(
                role = when (json.getString("role")) {
                    "leader" -> Role.Leader
                    "follower" -> Role.Follower
                    else -> Role.Off
                },
                status = json.getString("status"),
                identity = DeviceIdentity(json.getString("deviceID").toID(), json.getString("deviceName")),
                revision = json.getLong("revision"),
                connectedLeader = json.optStringOrNull("connectedLeader"),
                followerState = json.optStringOrNull("followerState"),
                isAudioConnected = json.getBoolean("isAudioConnected"),
                remoteLayout = SpeakerLayout.of(json.getInt("remoteLayout")),
                rounds = json.getJSONArray("rounds").objects().map(::parseRound),
                latestRound = json.optJSONObject("latestRound")?.let(::parseRound),
                leaderPort = json.getInt("leaderPort"),
                followerCount = json.getInt("followerCount"),
                members = json.getJSONArray("members").objects().map {
                    Member(it.getString("id").toID(), it.getString("name"), it.getBoolean("isActive"))
                },
                layout = SpeakerLayout.of(json.getInt("layout")),
                assignments = json.getJSONArray("assignments").objects().associate {
                    SpeakerChannel.of(it.getInt("channel")) to it.getString("device").toID()
                },
                activeChannels = json.getJSONArray("activeChannels").objects().associate {
                    it.getString("device").toID() to SpeakerChannel.of(it.getInt("channel"))
                },
                isLayoutComplete = json.getBoolean("isLayoutComplete"),
                isPreparingPlayback = json.getBoolean("isPreparingPlayback"),
                selectedTrack = json.optJSONObject("selectedTrack")?.let {
                    Track(it.getString("id").toID(), it.getString("name"), it.getDouble("duration"))
                },
                source = json.getString("source").toID(),
                isPlaying = json.getBoolean("isPlaying"),
                sourceDescription = json.getString("sourceDescription"),
                channel = SpeakerChannel.of(json.getInt("channel")),
                hasClockMap = json.getBoolean("hasClockMap"),
                isWaitingForAudio = json.getBoolean("isWaitingForAudio"),
                log = json.getJSONArray("log").objects().map { LogLine(it.getLong("date"), it.getString("text")) },
            )
        }

        private fun parseRound(json: JSONObject) = Round(
            index = json.getInt("index"),
            verdict = json.getString("verdict"),
            isAccepted = json.getBoolean("accepted"),
            probesSent = json.getInt("probesSent"),
            probesReceived = json.getInt("probesReceived"),
            bestRoundTrip = json.optLongOrNull("bestRoundTrip"),
            residual = json.optLongOrNull("residual"),
            offset = json.optLongOrNull("offset"),
            skewPPM = if (json.isNull("skewPPM")) null else json.getDouble("skewPPM"),
            minimumRoundTrip = json.getLong("minimumRoundTrip"),
            residualRMS = json.getDouble("residualRMS"),
            acceptedCount = json.getInt("acceptedCount"),
            rejectedCount = json.getInt("rejectedCount"),
            windowCount = json.getInt("windowCount"),
        )
    }
}

/** `asyncmusic::RenderStats`. */
data class RenderStats(
    val hostTimeValid: Boolean = true,
    val isPlaying: Boolean = false,
    val errorMicroseconds: Double = 0.0,
    val correctionPPM: Double = 0.0,
    val jumps: Int = 0,
    val mediaSeconds: Double = 0.0,
    val bufferedSeconds: Double = 0.0,
    val gaps: Int = 0,
    val outputPeak: Double = 0.0,
)

/** `AudioOutput::Status` (Android). */
data class OutputStatus(
    val isRunning: Boolean = false,
    val error: String = "",
    val deviceID: Int = 0,
    val sampleRate: Double = 0.0,
    val channels: Int = 0,
    val bufferDuration: Double = 0.0,
    val burstDuration: Double = 0.0,
    val latency: Double = 0.0,
    val timestampJitter: Double = 0.0,
    val lowLatency: Boolean = false,
    val underruns: Int = 0,
    val manualOffset: Double = 0.0,
)

/** What changes continuously, as `Engine::liveJSON` writes it. */
data class Live(
    val stats: RenderStats = RenderStats(),
    val output: OutputStatus = OutputStatus(),
    /** The leader's host clock now, in nanoseconds. */
    val leaderClock: Long? = null,
) {
    companion object {
        fun parse(text: String): Live {
            val json = JSONObject(text)
            val stats = json.getJSONObject("stats")
            val output = json.getJSONObject("output")
            return Live(
                stats = RenderStats(
                    hostTimeValid = stats.getBoolean("hostTimeValid"),
                    isPlaying = stats.getBoolean("isPlaying"),
                    errorMicroseconds = stats.optDouble("errorMicroseconds", 0.0),
                    correctionPPM = stats.optDouble("correctionPPM", 0.0),
                    jumps = stats.getInt("jumps"),
                    mediaSeconds = stats.optDouble("mediaSeconds", 0.0),
                    bufferedSeconds = stats.optDouble("bufferedSeconds", 0.0),
                    gaps = stats.getInt("gaps"),
                    outputPeak = stats.optDouble("outputPeak", 0.0),
                ),
                output = OutputStatus(
                    isRunning = output.getBoolean("isRunning"),
                    error = output.getString("error"),
                    deviceID = output.getInt("deviceID"),
                    sampleRate = output.optDouble("sampleRate", 0.0),
                    channels = output.getInt("channels"),
                    bufferDuration = output.optDouble("bufferDuration", 0.0),
                    burstDuration = output.optDouble("burstDuration", 0.0),
                    latency = output.optDouble("latency", 0.0),
                    timestampJitter = output.optDouble("timestampJitter", 0.0),
                    lowLatency = output.getBoolean("lowLatency"),
                    underruns = output.getInt("underruns"),
                    manualOffset = output.optDouble("manualOffset", 0.0),
                ),
                leaderClock = json.optLongOrNull("leaderClock"),
            )
        }
    }
}

/** IDs travel as 16 hex digits, since they use all 64 bits. */
private fun String.toID(): Long = java.lang.Long.parseUnsignedLong(this, 16)

private fun JSONObject.optStringOrNull(name: String): String? = if (isNull(name)) null else getString(name)

private fun JSONObject.optLongOrNull(name: String): Long? = if (isNull(name)) null else getLong(name)

private fun JSONArray.objects(): List<JSONObject> = List(length()) { getJSONObject(it) }
