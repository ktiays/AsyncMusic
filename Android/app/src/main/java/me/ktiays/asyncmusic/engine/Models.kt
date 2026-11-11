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

/** Whether this device listens for shares or shares its own audio; it does one or the other. */
enum class ShareMode(val title: String) {
    Listen("Listen"),
    Share("Share"),
}

/** Matches `asyncmusic::ShareSourceKind`: what kind of audio a share plays. */
enum class ShareSourceKind(val raw: Int) {
    Unknown(0),
    Application(1),
    File(2),
    System(3);

    companion object {
        fun of(raw: Int) = entries.firstOrNull { it.raw == raw } ?: Unknown
    }
}

/** Matches `asyncmusic::ShareDeviceKind`: what kind of device shares. */
enum class ShareDeviceKind(val raw: Int) {
    Unknown(0),
    Laptop(1),
    Desktop(2),
    Phone(3),
    Tablet(4);

    companion object {
        fun of(raw: Int) = entries.firstOrNull { it.raw == raw } ?: Unknown
    }
}

/** How a joined share's clock sync is going. */
enum class SyncStatus { Connecting, Syncing, Reconnecting, Failed }

/** A share heard on the network (`ShareCenter::Share`). */
data class NearbyShare(
    /** A new ID each time the device starts sharing; 0 for a share joined by address. */
    val id: Long,
    val device: Long,
    val name: String,
    val source: String,
    val sourceKind: ShareSourceKind,
    val deviceKind: ShareDeviceKind,
    val model: String,
    val listenerCount: Int,
    /** Where its clock leader is, "host:port". */
    val address: String,
    /** "Music · 2 listening": what it plays and how many listen. */
    val summary: String,
) {
    companion object {
        fun parse(json: JSONObject) = NearbyShare(
            id = json.getString("id").toID(),
            device = json.getString("device").toID(),
            name = json.getString("name"),
            source = json.getString("source"),
            sourceKind = ShareSourceKind.of(json.getInt("sourceKind")),
            deviceKind = ShareDeviceKind.of(json.getInt("deviceKind")),
            model = json.getString("model"),
            listenerCount = json.getInt("listenerCount"),
            address = json.getString("address"),
            summary = json.getString("summary"),
        )

        fun parse(text: String) = parse(JSONObject(text))
    }
}

/** A device listening to this device's share. */
data class ShareListener(val id: Long, val name: String, val channel: SpeakerChannel?)

/** What this device shares. */
data class ShareSource(val name: String, val kind: ShareSourceKind = ShareSourceKind.File)

/** The Listen/Share model (`ShareCenter::State`), as `Engine::stateJSON` writes it. */
data class ShareState(
    val mode: ShareMode = ShareMode.Listen,
    val deviceName: String = "",
    /** Every share heard, including the one joined. */
    val nearby: List<NearbyShare> = emptyList(),
    /** New shares waiting for an answer, newest first. */
    val alerts: List<NearbyShare> = emptyList(),
    val hasUnseenShares: Boolean = false,
    val discoveryError: String? = null,
    // Listening
    val joined: NearbyShare? = null,
    val isPlaying: Boolean = false,
    /** What this device plays of the joined share. */
    val channel: SpeakerChannel = SpeakerChannel.Full,
    /** The joined share's layout: in a stereo pair, this device picks Left or Right. */
    val joinedLayout: SpeakerLayout = SpeakerLayout.Everywhere,
    val syncStatus: SyncStatus = SyncStatus.Connecting,
    // Sharing
    val source: ShareSource? = null,
    val isSharing: Boolean = false,
    /** Why sharing isn't playing yet ("Loading…"), or null when it is. */
    val shareProblem: String? = null,
    val layout: SpeakerLayout = SpeakerLayout.Everywhere,
    val listeners: List<ShareListener> = emptyList(),
    val revision: Long = 0,
) {
    /** The shares nearby other than the one joined. */
    val nearbyOthers: List<NearbyShare> get() = nearby.filter { it.id != joined?.id }

    /** Whether this device is in a group, listening or sharing. */
    val isActive: Boolean get() = joined != null || isSharing

    /** Whether switching to [mode] would cut a connection, so the user should confirm. */
    fun switchInterrupts(mode: ShareMode) = mode != this.mode && isActive

    companion object {
        fun parse(text: String): ShareState {
            val json = JSONObject(text)
            return ShareState(
                mode = if (json.getString("mode") == "share") ShareMode.Share else ShareMode.Listen,
                deviceName = json.getString("deviceName"),
                nearby = json.getJSONArray("nearby").objects().map(NearbyShare::parse),
                alerts = json.getJSONArray("alerts").objects().map(NearbyShare::parse),
                hasUnseenShares = json.getBoolean("hasUnseenShares"),
                discoveryError = json.getString("discoveryError").ifEmpty { null },
                joined = json.optJSONObject("joined")?.let(NearbyShare::parse),
                isPlaying = json.getBoolean("isPlaying"),
                channel = SpeakerChannel.of(json.getInt("channel")),
                joinedLayout = SpeakerLayout.of(json.getInt("joinedLayout")),
                syncStatus = when (json.getString("syncStatus")) {
                    "syncing" -> SyncStatus.Syncing
                    "reconnecting" -> SyncStatus.Reconnecting
                    "failed" -> SyncStatus.Failed
                    else -> SyncStatus.Connecting
                },
                source = json.optJSONObject("source")?.let { ShareSource(it.getString("name"), ShareSourceKind.of(it.getInt("kind"))) },
                isSharing = json.getBoolean("isSharing"),
                shareProblem = json.getString("shareProblem").ifEmpty { null },
                layout = SpeakerLayout.of(json.getInt("layout")),
                listeners = json.getJSONArray("listeners").objects().map {
                    ShareListener(it.getString("id").toID(), it.getString("name"), if (it.isNull("channel")) null else SpeakerChannel.of(it.getInt("channel")))
                },
                revision = json.getLong("revision"),
            )
        }
    }
}

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
