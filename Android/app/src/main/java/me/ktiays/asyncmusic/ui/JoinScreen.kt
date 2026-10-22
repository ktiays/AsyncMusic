//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.PlayCircle
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.LeaderBrowser
import me.ktiays.asyncmusic.engine.OutputStatus
import me.ktiays.asyncmusic.engine.Snapshot
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout

/** A follower's screen: pick a host nearby, then show what this device plays for it. */
@Composable
fun JoinScreen(
    state: Snapshot,
    leaders: List<LeaderBrowser.Leader>,
    browseError: String?,
    output: OutputStatus,
    onFollow: (String) -> Unit,
    onFollowAddress: (String, Int) -> Unit,
    onLeave: () -> Unit,
) {
    ReadableColumn(Modifier.verticalScroll(rememberScrollState())) {
        val host = state.connectedLeader
        if (host != null) {
            SpeakerStatus(state, host)
            if (output.error.isNotEmpty()) Banner("Audio output", output.error)
        } else {
            HostList(leaders, browseError, onFollow)
            AddressEntry(onFollowAddress)
        }
        OutlinedButton(onClick = onLeave, Modifier.align(Alignment.CenterHorizontally)) {
            Text(if (host == null) "Cancel" else "Leave", color = MaterialTheme.colorScheme.error)
        }
    }
}

@Composable
private fun HostList(leaders: List<LeaderBrowser.Leader>, error: String?, onFollow: (String) -> Unit) {
    SectionCard("Nearby Hosts") {
        if (leaders.isEmpty()) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                if (error == null) CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                Text(error ?: "Looking for hosts…", color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Text(
                "On the device with the music, open AsyncMusic and choose Host Music. Both devices need to be on the same network.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        leaders.forEachIndexed { index, leader ->
            if (index > 0) Divider()
            ChoiceRow(Icons.Filled.PlayCircle, leader.name, "Tap to join", showsChevron = true) { onFollow(leader.name) }
        }
    }
}

/** For networks that keep devices from finding each other: join a host by its address. */
@Composable
private fun AddressEntry(onFollowAddress: (String, Int) -> Unit) {
    var isExpanded by rememberSaveable { mutableStateOf(false) }
    var address by rememberSaveable { mutableStateOf("") }
    var port by rememberSaveable { mutableStateOf("") }
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        TextButton(onClick = { isExpanded = !isExpanded }) {
            Text(if (isExpanded) "Hide" else "Host not listed? Join by address")
        }
        AnimatedVisibility(isExpanded) {
            SectionCard {
                Text(
                    "The host's diagnostics show its clock port. Both devices still need to reach each other on the network.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        address,
                        { address = it.trim() },
                        Modifier.weight(1f),
                        label = { Text("Address") },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Next),
                    )
                    OutlinedTextField(
                        port,
                        { port = it.filter(Char::isDigit).take(5) },
                        Modifier.width(96.dp),
                        label = { Text("Port") },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
                    )
                }
                val portNumber = port.toIntOrNull()
                OutlinedButton(
                    onClick = { onFollowAddress(address, portNumber ?: 0) },
                    enabled = address.isNotEmpty() && portNumber != null && portNumber in 1..65535,
                    modifier = Modifier.align(Alignment.End),
                ) { Text("Join") }
            }
        }
    }
}

@Composable
private fun SpeakerStatus(state: Snapshot, host: String) {
    val channel = state.channel
    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 12.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Box {
            SpeakerBadge(channel, 120.dp)
            if (state.isPlaying) {
                Surface(Modifier.align(Alignment.BottomEnd).offset(6.dp, 6.dp), shape = CircleShape, color = MaterialTheme.colorScheme.background) {
                    Icon(Icons.AutoMirrored.Filled.VolumeUp, contentDescription = "Playing", Modifier.padding(8.dp), tint = MaterialTheme.colorScheme.primary)
                }
            }
        }
        Text(
            when (channel) {
                SpeakerChannel.Left -> "Left Speaker"
                SpeakerChannel.Right -> "Right Speaker"
                SpeakerChannel.Full -> "Speaker"
            },
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold,
        )
        Text(
            when {
                channel != SpeakerChannel.Full ->
                    "This device plays the ${if (channel == SpeakerChannel.Left) "left" else "right"} channel of a stereo pair with $host."
                state.remoteLayout == SpeakerLayout.StereoPair -> "Plays the full mix until both sides of the stereo pair are connected."
                else -> "This device plays the full mix in sync with $host."
            },
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
    }
    SectionCard {
        LabeledRow("Host", host)
        Divider()
        LabeledRow("Sync") {
            val (text, tone) = when (state.followerState) {
                "syncing" -> if (state.hasClockMap) "In sync" to Tone.Good else "Syncing…" to Tone.Pending
                "reconnecting" -> "Reconnecting…" to Tone.Pending
                "failed" -> "Connection failed" to Tone.Bad
                else -> "Connecting…" to Tone.Pending
            }
            StatusLabel(text, tone)
        }
        Divider()
        LabeledRow("Playback") {
            when {
                !state.isPlaying -> StatusLabel("Waiting for the host", Tone.Neutral)
                state.isWaitingForAudio -> StatusLabel("Receiving audio…", Tone.Pending)
                else -> StatusLabel("Playing", Tone.Good)
            }
        }
    }
}

/** A problem worth knowing about, e.g. that the audio output could not be opened. */
@Composable
fun Banner(title: String, message: String) {
    Surface(Modifier.fillMaxWidth(), shape = MaterialTheme.shapes.medium, color = MaterialTheme.colorScheme.errorContainer) {
        Column(Modifier.padding(14.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.onErrorContainer)
            Text(message, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onErrorContainer)
        }
    }
}
