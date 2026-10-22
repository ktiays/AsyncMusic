//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.Live
import me.ktiays.asyncmusic.engine.OutputStatus
import me.ktiays.asyncmusic.engine.Role
import me.ktiays.asyncmusic.engine.Round
import me.ktiays.asyncmusic.engine.Snapshot
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.max

/** What the diagnostics screen can change. */
interface DiagnosticsActions {
    fun setManualOffset(milliseconds: Double)
    fun rename(name: String)
    fun copyReport()
    fun clearLog()
}

/** Everything about the clock sync, playback and output, for finding out why devices drift apart. */
@Composable
fun DiagnosticsScreen(
    state: Snapshot,
    live: Live,
    discoveredHosts: Int,
    advertisingError: String?,
    customName: String,
    actions: DiagnosticsActions,
) {
    val context = LocalContext.current
    ReadableColumn(Modifier.verticalScroll(rememberScrollState())) {
        SectionCard("General") {
            LabeledRow("Role", roleTitle(state.role))
            LabeledRow("Device", state.identity.name)
            LabeledRow("Device ID", "%016x".format(state.identity.id))
            LabeledRow("Status", state.status)
        }

        if (state.role == Role.Leader) {
            SectionCard("Group") {
                LabeledRow("Followers", state.followerCount.toString())
                LabeledRow("Members", state.members.joinToString("\n") { it.name + if (it.isActive) "" else " (not responding)" }.ifEmpty { "–" })
                LabeledRow("Clock port (UDP)", if (state.leaderPort != 0) state.leaderPort.toString() else "–")
                LabeledRow("Advertising", advertisingError ?: if (state.leaderPort != 0) "As “${state.identity.name}”" else "–")
            }
        }

        if (state.role == Role.Follower) {
            SectionCard("Clock") {
                LabeledRow("Connected to", state.connectedLeader ?: "–")
                LabeledRow("Discovered hosts", discoveredHosts.toString())
                val round = state.latestRound
                LabeledRow("Offset", round?.offset?.let { "%.3f ms".format(it / 1e6) } ?: "–")
                LabeledRow("Skew", round?.skewPPM?.let { "%+.2f ppm".format(it) } ?: "–")
                LabeledRow("Leader clock") {
                    Text(
                        live.leaderClock?.let { "%03d.%03d s".format(it / 1_000_000_000 % 1000, it / 1_000_000 % 1000) } ?: "–",
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                LabeledRow("Fit jitter (RMS)", round?.let { "%.0f µs".format(it.residualRMS / 1e3) } ?: "–")
                LabeledRow("Min RTT", round?.minimumRoundTrip?.takeIf { it > 0 }?.let { "%.2f ms".format(it / 1e6) } ?: "–")
                LabeledRow("Asymmetry bound", round?.minimumRoundTrip?.takeIf { it > 0 }?.let { "± %.2f ms".format(it / 2e6) } ?: "–")
                LabeledRow("Rounds", round?.let { "${it.acceptedCount} accepted · ${it.rejectedCount} rejected · ${it.windowCount} in window" } ?: "–")
                LabeledRow("Last round", round?.let { "${it.probesReceived}/${it.probesSent} replies · ${it.verdict}" } ?: "–")
            }
            if (state.rounds.isNotEmpty()) RoundCharts(state.rounds)
        }

        if (state.role != Role.Off) {
            val stats = live.stats
            SectionCard("Speakers and Playback") {
                LabeledRow("Layout", layoutTitle(if (state.role == Role.Leader) state.layout else state.remoteLayout))
                LabeledRow("This device plays", channelTitle(state.channel))
                LabeledRow("Now", if (state.isPlaying) state.sourceDescription + if (state.isWaitingForAudio) " (waiting for audio)" else "" else "Stopped")
                LabeledRow("Position", if (stats.isPlaying) "%.3f s".format(stats.mediaSeconds) else "–")
                LabeledRow("Schedule error", if (stats.isPlaying) "%+.0f µs".format(stats.errorMicroseconds) else "–")
                LabeledRow("Rate correction", "%+.1f ppm".format(stats.correctionPPM))
                LabeledRow("Repositions", stats.jumps.toString())
                LabeledRow("Stream gaps", stats.gaps.toString())
                LabeledRow(
                    "Output level",
                    when {
                        !stats.isPlaying -> "–"
                        stats.outputPeak > 0 -> "%.1f dBFS".format(20 * log10(stats.outputPeak))
                        else -> "silent"
                    },
                )
                LabeledRow("Buffered ahead", if (stats.isPlaying && stats.bufferedSeconds > 0) "%.1f s".format(stats.bufferedSeconds) else "–")
            }
            val output = live.output
            SectionCard("Output") {
                LabeledRow("Device", outputDeviceName(context, output.deviceID) ?: "–")
                LabeledRow("State", if (output.isRunning) "Running" else output.error.ifEmpty { "Stopped" })
                LabeledRow("Sample rate", if (output.sampleRate > 0) "%.0f Hz".format(output.sampleRate) else "–")
                LabeledRow("Buffer", if (output.bufferDuration > 0) "%.1f ms (bursts of %.1f ms)".format(output.bufferDuration * 1e3, output.burstDuration * 1e3) else "–")
                LabeledRow("Latency", if (output.isRunning) "%.1f ms".format(output.latency * 1e3) else "–")
                LabeledRow("Timestamp jitter", "%.0f µs".format(output.timestampJitter))
                LabeledRow("Underruns", output.underruns.toString())
            }
        }

        ManualOffsetCard(live.output.manualOffset, actions)
        DeviceNameCard(customName, state.identity.name, actions)
        LogCard(state, actions)
    }
}

@Composable
private fun RoundCharts(rounds: List<Round>) {
    val accepted = MaterialTheme.colorScheme.primary
    val rejected = Tone.Pending.color()
    val axis = MaterialTheme.colorScheme.outlineVariant
    val first = rounds.first().index
    val last = max(rounds.last().index, first + 1)
    SectionCard {
        // Residuals around the fitted line, in µs; orange for rounds the estimator rejected.
        val limit = rounds.mapNotNull { it.residual }.maxOfOrNull { abs(it) / 1e3 }?.coerceAtLeast(100.0) ?: 100.0
        ChartTitle("Residual vs. fitted line (µs)", "±%.0f".format(limit))
        Canvas(Modifier.fillMaxWidth().height(90.dp)) {
            val x = { index: Int -> (index - first) * (size.width - 8f) / (last - first) + 4f }
            drawLine(axis, Offset(0f, size.height / 2), Offset(size.width, size.height / 2))
            rounds.forEach { round ->
                val residual = round.residual ?: return@forEach
                val y = size.height / 2 - (residual / 1e3 / limit).toFloat() * (size.height / 2 - 4f)
                drawCircle(if (round.isAccepted) accepted else rejected, radius = 2.5.dp.toPx(), center = Offset(x(round.index), y))
            }
        }
        // Best round trip per round, in ms.
        val maximum = (rounds.mapNotNull { it.bestRoundTrip }.maxOfOrNull { it / 1e6 } ?: 1.0).coerceAtLeast(1.0) * 1.1
        ChartTitle("Best round trip per round (ms)", "%.1f".format(maximum))
        Canvas(Modifier.fillMaxWidth().height(70.dp)) {
            val x = { index: Int -> (index - first) * (size.width - 8f) / (last - first) + 4f }
            val path = Path()
            var started = false
            rounds.forEach { round ->
                val trip = round.bestRoundTrip ?: return@forEach
                val point = Offset(x(round.index), size.height - (trip / 1e6 / maximum).toFloat() * size.height)
                if (started) path.lineTo(point.x, point.y) else path.moveTo(point.x, point.y)
                started = true
            }
            drawLine(axis, Offset(0f, size.height), Offset(size.width, size.height))
            drawPath(path, accepted, style = Stroke(width = 1.5.dp.toPx()))
        }
    }
}

@Composable
private fun ChartTitle(title: String, scale: String) {
    Row {
        Text(title, style = MaterialTheme.typography.labelMedium)
        Spacer(Modifier.weight(1f))
        Text(scale, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
private fun ManualOffsetCard(offset: Double, actions: DiagnosticsActions) {
    // The slider moves freely; the engine follows.
    var value by remember { mutableFloatStateOf(offset.toFloat()) }
    SectionCard("Extra delay") {
        Row {
            Text("%+.1f ms".format(value), style = MaterialTheme.typography.titleMedium, fontFamily = FontFamily.Monospace)
            Spacer(Modifier.weight(1f))
            TextButton(onClick = {
                value = 0f
                actions.setManualOffset(0.0)
            }, enabled = value != 0f) { Text("Reset") }
        }
        Slider(
            value = value,
            onValueChange = {
                value = (Math.round(it * 2) / 2.0).toFloat()
                actions.setManualOffset(value.toDouble())
            },
            valueRange = -100f..300f,
        )
        Text(
            "Increase if this device sounds late compared with the others; decrease if it sounds early.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun DeviceNameCard(customName: String, currentName: String, actions: DiagnosticsActions) {
    var name by remember { mutableStateOf(customName) }
    SectionCard("Device Name") {
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                name,
                { name = it },
                Modifier.weight(1f),
                placeholder = { Text(currentName) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { actions.rename(name.trim()) }),
            )
            OutlinedButton(onClick = { actions.rename(name.trim()) }, enabled = name.trim() != customName) { Text("Save") }
        }
        Text(
            "How hosts list this device. Leave empty to use the device's name. Takes effect the next time you join.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun LogCard(state: Snapshot, actions: DiagnosticsActions) {
    SectionCard("Log") {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = actions::copyReport) { Text("Copy Report") }
            OutlinedButton(onClick = actions::clearLog) { Text("Clear") }
        }
        // Newest first.
        val lines = state.log.takeLast(60).asReversed().joinToString("\n") { "${timeOfDay(it.date)}  ${it.text}" }
        SelectionContainer {
            Text(
                lines.ifEmpty { "Nothing yet." },
                Modifier.fillMaxWidth(),
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Start,
            )
        }
    }
}

// MARK: - Text

fun roleTitle(role: Role) = when (role) {
    Role.Off -> "Off"
    Role.Leader -> "Leader"
    Role.Follower -> "Follower"
}

fun layoutTitle(layout: SpeakerLayout) = when (layout) {
    SpeakerLayout.Everywhere -> "Everywhere"
    SpeakerLayout.StereoPair -> "Stereo Pair"
}

fun channelTitle(channel: SpeakerChannel) = when (channel) {
    SpeakerChannel.Full -> "Full Mix"
    SpeakerChannel.Left -> "Left"
    SpeakerChannel.Right -> "Right"
}

fun timeOfDay(date: Long): String = SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(Date(date))

/** The output AAudio plays on, e.g. "Pixel Buds Pro (Bluetooth)". */
fun outputDeviceName(context: Context, id: Int): String? {
    if (id == 0) return null
    val device = context.getSystemService(AudioManager::class.java).getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.id == id } ?: return null
    val kind = when (device.type) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Speaker"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Wired"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth"
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        else -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && device.type == AudioDeviceInfo.TYPE_BLE_HEADSET) "Bluetooth LE" else null
    }
    val name = device.productName?.toString()?.takeIf { it.isNotBlank() } ?: kind ?: "Output"
    return if (kind != null && kind != name) "$name ($kind)" else name
}

/** The diagnostics as text, for pasting into a bug report. */
fun diagnosticsReport(context: Context, state: Snapshot, live: Live): String = buildString {
    val output: OutputStatus = live.output
    val stats = live.stats
    appendLine("AsyncMusic diagnostics (Android ${Build.VERSION.RELEASE}, ${Build.MANUFACTURER} ${Build.MODEL})")
    appendLine("Device: ${state.identity.name} (%016x)".format(state.identity.id))
    appendLine("Role: ${roleTitle(state.role)} · ${state.status}")
    appendLine(
        "Output: ${outputDeviceName(context, output.deviceID) ?: "?"} · ${if (output.isRunning) "running" else "stopped ${output.error}"} · " +
            "%.0f Hz, buffer %.1f ms, burst %.1f ms, latency %.1f ms, timestamp jitter %.0f µs, underruns %d, extra delay %+.1f ms".format(
                output.sampleRate, output.bufferDuration * 1e3, output.burstDuration * 1e3, output.latency * 1e3, output.timestampJitter,
                output.underruns, output.manualOffset,
            ),
    )
    appendLine(
        "Playing: ${if (state.isPlaying) state.sourceDescription else "nothing"}${if (state.isWaitingForAudio) " (waiting for audio)" else ""} · " +
            "channel ${channelTitle(state.channel)}",
    )
    appendLine(
        "Render: playing ${if (stats.isPlaying) "yes" else "no"}, position %.3f s, error %+.0f µs, correction %+.1f ppm, buffered %.1f s, gaps %d, repositions %d, peak %.4f, host time %s".format(
            stats.mediaSeconds, stats.errorMicroseconds, stats.correctionPPM, stats.bufferedSeconds, stats.gaps, stats.jumps, stats.outputPeak,
            if (stats.hostTimeValid) "valid" else "INVALID",
        ),
    )
    appendLine()
    appendLine("Log (oldest first):")
    state.log.forEach { appendLine("${timeOfDay(it.date)}  ${it.text}") }
}
