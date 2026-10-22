//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.Hearing
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.PhoneAndroid
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material.icons.filled.SpeakerGroup
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.UnfoldMore
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.Live
import me.ktiays.asyncmusic.engine.PlaybackSource
import me.ktiays.asyncmusic.engine.Snapshot
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout

/** What the host's screen can do. */
interface HostActions {
    fun chooseMusic()
    fun play(source: Long)
    fun stopPlayback()
    fun setLayout(layout: SpeakerLayout)
    fun assign(device: Long, channel: SpeakerChannel)
    fun swapChannels()
    fun stopHosting()
}

/** The leader's screen: what plays, how it is spread over the group, and who is in it. */
@Composable
fun HostScreen(state: Snapshot, live: Live, isLoadingTrack: Boolean, problem: String?, actions: HostActions) {
    ReadableColumn(Modifier.verticalScroll(rememberScrollState())) {
        if (problem != null) Banner("Something went wrong", problem)
        NowPlayingCard(state, live, isLoadingTrack, actions)
        SectionCard("Sound") {
            LayoutChoice(SpeakerLayout.Everywhere, state.layout, Icons.Filled.SpeakerGroup, "Everywhere", "Every device plays the full mix.", actions)
            LayoutChoice(SpeakerLayout.StereoPair, state.layout, Icons.Filled.Speaker, "Stereo Pair", "Two devices play the left and right channels.", actions)
        }
        if (state.layout == SpeakerLayout.StereoPair) StereoPairCard(state, actions)
        DevicesCard(state)
        OutlinedButton(onClick = actions::stopHosting, Modifier.align(Alignment.CenterHorizontally)) {
            Text("Stop Hosting", color = MaterialTheme.colorScheme.error)
        }
    }
}

@Composable
private fun LayoutChoice(layout: SpeakerLayout, selected: SpeakerLayout, icon: ImageVector, title: String, subtitle: String, actions: HostActions) {
    ChoiceRow(icon, title, subtitle, isSelected = layout == selected) { actions.setLayout(layout) }
}

@Composable
private fun NowPlayingCard(state: Snapshot, live: Live, isLoading: Boolean, actions: HostActions) {
    val isPlaying = state.isPlaying
    val track = state.selectedTrack
    val title = if (isPlaying) {
        when (state.source) {
            PlaybackSource.METRONOME -> "Metronome"
            PlaybackSource.CHANNEL_TEST -> "Left & Right Test"
            else -> track?.takeIf { it.id == state.source }?.name ?: "Music"
        }
    } else {
        track?.name ?: "No Music Chosen"
    }
    val subtitle = when {
        isLoading -> "Loading…"
        state.isPreparingPlayback -> "Getting speakers ready…"
        isPlaying -> {
            val devices = 1 + state.members.count { it.isActive }
            if (devices == 1) "Playing on this device" else "Playing on $devices devices"
        }
        track != null -> "Ready to play"
        else -> "Choose a song to start."
    }
    SectionCard {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            val primary = MaterialTheme.colorScheme.primary
            Box(
                Modifier
                    .size(64.dp)
                    .clip(RoundedCornerShape(14.dp))
                    .background(Brush.verticalGradient(listOf(primary.copy(alpha = 0.75f), primary))),
                contentAlignment = Alignment.Center,
            ) {
                if (isLoading) {
                    CircularProgressIndicator(Modifier.size(28.dp), color = Color.White, strokeWidth = 3.dp)
                } else {
                    Icon(if (isPlaying) Icons.Filled.GraphicEq else Icons.Filled.MusicNote, contentDescription = null, Modifier.size(32.dp), tint = Color.White)
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold, maxLines = 2, overflow = TextOverflow.Ellipsis)
                Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        // How far the playing file has got.
        if (isPlaying && track != null && track.id == state.source) {
            val duration = maxOf(track.duration, 1.0)
            val elapsed = if (live.stats.isPlaying) live.stats.mediaSeconds.coerceIn(0.0, duration) else 0.0
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                LinearProgressIndicator(progress = { (elapsed / duration).toFloat() }, Modifier.fillMaxWidth())
                Row {
                    val style = MaterialTheme.typography.labelSmall.copy(fontFamily = FontFamily.Monospace)
                    Text(minutesAndSeconds(elapsed), style = style, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.weight(1f))
                    Text("-" + minutesAndSeconds(duration - elapsed), style = style, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedButton(onClick = actions::chooseMusic, enabled = !isLoading) {
                Icon(Icons.AutoMirrored.Filled.QueueMusic, contentDescription = null, Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text("Choose Music")
            }
            Spacer(Modifier.weight(1f))
            val canStop = isPlaying || state.isPreparingPlayback
            FilledIconButton(
                onClick = { if (canStop) actions.stopPlayback() else track?.let { actions.play(it.id) } },
                enabled = canStop || track != null,
                modifier = Modifier.size(56.dp),
            ) {
                Icon(if (canStop) Icons.Filled.Stop else Icons.Filled.PlayArrow, contentDescription = if (canStop) "Stop" else "Play", Modifier.size(30.dp))
            }
        }
    }
}

@Composable
private fun StereoPairCard(state: Snapshot, actions: HostActions) {
    val isTesting = state.source == PlaybackSource.CHANNEL_TEST
    SectionCard("Stereo Pair") {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.Top) {
            Slot(state, SpeakerChannel.Left, actions, Modifier.weight(1f))
            FilledTonalIconButton(onClick = actions::swapChannels, Modifier.padding(top = 14.dp)) {
                Icon(Icons.Filled.SwapHoriz, contentDescription = "Swap Left and Right")
            }
            Slot(state, SpeakerChannel.Right, actions, Modifier.weight(1f))
        }
        if (state.isLayoutComplete) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = Tone.Good.color(), modifier = Modifier.size(20.dp))
                Text("Stereo is on. For the best effect, sit midway between the two devices.", style = MaterialTheme.typography.bodyMedium, color = Tone.Good.color())
            }
        } else {
            val waiting = when {
                state.members.isEmpty() ->
                    "Open AsyncMusic on a second device on this network and choose Join as Speaker. Until then, this device plays the full mix."
                else -> state.assignments.values.firstOrNull { !state.isDevicePresent(it) }?.let { device ->
                    "Waiting for ${state.nameOfDevice(device) ?: "the other device"}. Meanwhile every device plays the full mix."
                } ?: "Choose a device for each side. Meanwhile every device plays the full mix."
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Icon(Icons.Filled.Info, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.size(20.dp))
                Text(waiting, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        OutlinedButton(onClick = { if (isTesting) actions.stopPlayback() else actions.play(PlaybackSource.CHANNEL_TEST) }) {
            Icon(if (isTesting) Icons.Filled.Stop else Icons.Filled.Hearing, contentDescription = null, Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Text(if (isTesting) "Stop Test" else "Test Left & Right")
        }
    }
}

@Composable
private fun Slot(state: Snapshot, channel: SpeakerChannel, actions: HostActions, modifier: Modifier) {
    val device = state.assignments[channel]
    val isPresent = device != null && state.isDevicePresent(device)
    val candidates = listOf(state.identity.id to state.identity.name) + state.members.map { it.id to it.name }
    var isChoosing by remember { mutableStateOf(false) }
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        SpeakerBadge(channel, 60.dp, isDimmed = !isPresent)
        Box {
            TextButton(onClick = { isChoosing = true }) {
                Text(device?.let(state::nameOfDevice) ?: "Choose Device", maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                Icon(Icons.Filled.UnfoldMore, contentDescription = null, Modifier.size(16.dp))
            }
            DropdownMenu(expanded = isChoosing, onDismissRequest = { isChoosing = false }) {
                candidates.forEach { (id, name) ->
                    DropdownMenuItem(
                        text = { Text(name) },
                        onClick = {
                            isChoosing = false
                            actions.assign(id, channel)
                        },
                        trailingIcon = if (id == device) {
                            { Icon(Icons.Filled.CheckCircle, contentDescription = "Chosen", Modifier.size(18.dp)) }
                        } else {
                            null
                        },
                    )
                }
            }
        }
        Text(
            when {
                device == null -> " "
                device == state.identity.id -> "This device"
                isPresent -> "Connected"
                else -> "Not connected"
            },
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun DevicesCard(state: Snapshot) {
    SectionCard("Devices") {
        DeviceRow(state.identity.name, "This device · Host", Icons.Filled.PhoneAndroid, true, state.channel)
        state.members.forEach { member ->
            Divider()
            DeviceRow(
                member.name,
                if (member.isActive) "Connected" else "Not responding",
                Icons.Filled.Speaker,
                member.isActive,
                state.activeChannels[member.id] ?: SpeakerChannel.Full,
            )
        }
        if (state.members.isEmpty()) {
            Text(
                "No other devices yet. Open AsyncMusic on another device on this network and choose Join as Speaker.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun DeviceRow(name: String, detail: String, icon: ImageVector, isActive: Boolean, channel: SpeakerChannel) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.width(28.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(name, maxLines = 1, overflow = TextOverflow.Ellipsis)
            StatusLabel(detail, if (isActive) Tone.Good else Tone.Pending)
        }
        SpeakerBadge(channel, 30.dp, isDimmed = !isActive)
    }
}

fun minutesAndSeconds(seconds: Double): String {
    val total = seconds.coerceAtLeast(0.0).toInt()
    return "%d:%02d".format(total / 60, total % 60)
}
