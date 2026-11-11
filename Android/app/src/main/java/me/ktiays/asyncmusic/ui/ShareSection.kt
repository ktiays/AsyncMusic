//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AudioFile
import androidx.compose.material.icons.rounded.MusicNote
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Sensors
import androidx.compose.material.icons.rounded.Speaker
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.WarningAmber
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.LinearWavyProgressIndicator
import androidx.compose.material3.LoadingIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.GroupController.SourceStatus
import me.ktiays.asyncmusic.engine.ShareState
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout

/** What the Share screens can do. */
interface ShareActions {
    fun chooseFile()
    fun startSharing()
    fun stopSharing()
    fun setLayout(layout: SpeakerLayout)
    /** Plays the file from the top for everyone, or stops it. */
    fun play()
    fun stop()
}

/** Where a shared file is: seconds played of how many, while it plays. */
data class Progress(val isPlaying: Boolean, val elapsed: Double, val duration: Double)

/** Choosing a file and starting a share. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ShareSetup(state: ShareState, sourceStatus: SourceStatus, actions: ShareActions) {
    val colors = shareColors()
    val source = state.source
    Column(verticalArrangement = Arrangement.spacedBy(20.dp)) {
        state.discoveryError?.let { Notice(Icons.Rounded.WarningAmber, it) }
        if (source == null) {
            // A big tonal area, dashed around, to tap.
            Column(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(32.dp))
                    .background(colors.tint)
                    .border(2.dp, colors.accent.copy(alpha = 0.5f), RoundedCornerShape(32.dp))
                    .clickable(onClickLabel = "Choose an audio file", role = Role.Button, onClick = actions::chooseFile)
                    .padding(vertical = 40.dp, horizontal = 24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Icon(Icons.Outlined.AudioFile, contentDescription = null, tint = colors.accent, modifier = Modifier.size(48.dp))
                Text("Choose an audio file", style = MaterialTheme.typography.titleLargeEmphasized)
                Text("MP3, AAC, FLAC, WAV and more", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        } else {
            Surface(shape = RoundedCornerShape(28.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxWidth()) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Artwork(56)
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(source.name, style = MaterialTheme.typography.titleMediumEmphasized, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(
                            when {
                                sourceStatus.isLoading -> "Loading…"
                                sourceStatus.error != null -> sourceStatus.error
                                else -> "Audio file"
                            },
                            style = MaterialTheme.typography.bodyMedium,
                            color = if (sourceStatus.error != null) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2,
                        )
                    }
                    if (sourceStatus.isLoading) {
                        LoadingIndicator(Modifier.size(40.dp), color = colors.accent)
                    } else {
                        OutlinedButton(onClick = actions::chooseFile, shapes = ButtonDefaults.shapes()) { Text("Change") }
                    }
                }
            }
        }

        Button(
            onClick = actions::startSharing,
            enabled = source != null && sourceStatus.error == null,
            shapes = ButtonDefaults.shapesFor(ButtonDefaults.MediumContainerHeight),
            contentPadding = ButtonDefaults.contentPaddingFor(ButtonDefaults.MediumContainerHeight),
            colors = ButtonDefaults.buttonColors(containerColor = colors.container, contentColor = colors.onContainer),
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = ButtonDefaults.MediumContainerHeight),
        ) {
            Icon(Icons.Rounded.Sensors, contentDescription = null, modifier = Modifier.size(ButtonDefaults.iconSizeFor(ButtonDefaults.MediumContainerHeight)))
            Spacer(Modifier.size(ButtonDefaults.iconSpacingFor(ButtonDefaults.MediumContainerHeight)))
            Text("Start Sharing", style = ButtonDefaults.textStyleFor(ButtonDefaults.MediumContainerHeight))
        }

        Text(
            "Everyone on this network will get an alert.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth(),
        )
    }
}

/** The title of the live share: an orange dot with a soft halo, "Sharing", and the name seen. */
@Composable
fun SharingTitle(deviceName: String, modifier: Modifier = Modifier) {
    val colors = shareColors()
    val halo by rememberInfiniteTransition(label = "halo").animateFloat(
        initialValue = 1f,
        targetValue = 1.8f,
        animationSpec = infiniteRepeatable(tween(1400)),
        label = "halo",
    )
    Column(modifier) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            Box(Modifier.size(28.dp), contentAlignment = Alignment.Center) {
                Box(
                    Modifier
                        .size(14.dp)
                        .scale(halo)
                        .clip(CircleShape)
                        .background(colors.accent.copy(alpha = 0.35f * (1.8f - halo) / 0.8f)),
                )
                Box(
                    Modifier
                        .size(14.dp)
                        .clip(CircleShape)
                        .background(colors.accent),
                )
            }
            Text("Sharing", style = MaterialTheme.typography.displaySmallEmphasized)
        }
        Text("Seen as “$deviceName”", style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 42.dp), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

/** The share under way: the file and its transport, the sound layout and who listens. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun SharingLive(state: ShareState, progress: Progress, actions: ShareActions) {
    val colors = shareColors()
    Column(verticalArrangement = Arrangement.spacedBy(20.dp)) {
        state.discoveryError?.let { Notice(Icons.Rounded.WarningAmber, it) }
        Surface(shape = RoundedCornerShape(32.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Artwork(64)
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(state.source?.name ?: "Audio", style = MaterialTheme.typography.headlineSmallEmphasized, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(state.shareProblem ?: "Audio file", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    if (state.shareProblem?.endsWith("…") == true) LoadingIndicator(Modifier.size(40.dp), color = colors.accent)
                }
                val fraction = if (progress.duration > 0) (progress.elapsed / progress.duration).toFloat().coerceIn(0f, 1f) else 0f
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    LinearWavyProgressIndicator(
                        progress = { fraction },
                        color = colors.accent,
                        trackColor = colors.tint,
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Row {
                        Text(time(progress.elapsed), style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.weight(1f))
                        Text("−" + time((progress.duration - progress.elapsed).coerceAtLeast(0.0)), style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                // Play from the top, or stop, for everyone (Core has no pause or seek yet).
                FilledIconButton(
                    onClick = if (progress.isPlaying) actions::stop else actions::play,
                    shapes = IconButtonDefaults.shapes(),
                    colors = IconButtonDefaults.filledIconButtonColors(containerColor = colors.container, contentColor = colors.onContainer),
                    modifier = Modifier
                        .size(80.dp)
                        .align(Alignment.CenterHorizontally),
                ) {
                    Icon(
                        if (progress.isPlaying) Icons.Rounded.Stop else Icons.Rounded.PlayArrow,
                        contentDescription = if (progress.isPlaying) "Stop for everyone" else "Play for everyone",
                        modifier = Modifier.size(36.dp),
                    )
                }
            }
        }

        PanelSection("Sound") {
            item {
                ConnectedChoice(
                    SpeakerLayout.entries,
                    state.layout,
                    label = ::layoutTitle,
                    onSelect = actions::setLayout,
                    modifier = Modifier.padding(12.dp),
                )
            }
        }

        Surface(shape = RoundedCornerShape(28.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(vertical = 8.dp)) {
                Text(
                    if (state.listeners.isEmpty()) "Listening" else "Listening · ${state.listeners.size}",
                    style = MaterialTheme.typography.titleMediumEmphasized,
                    modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp, bottom = 4.dp),
                )
                if (state.listeners.isEmpty()) {
                    Text("No one has joined yet.", style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 20.dp, vertical = 12.dp))
                }
                state.listeners.forEach { listener ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .padding(horizontal = 16.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        Box(
                            Modifier
                                .size(44.dp)
                                .clip(CircleShape)
                                .background(MaterialTheme.colorScheme.surfaceContainerHighest),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(Icons.Rounded.Speaker, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        Text(listener.name, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        ChannelChip(listener.channel)
                    }
                }
            }
        }
    }
}

/** A listener's channel: "L" in a blue chip, "R" in an orange one, or "Both". */
@Composable
fun ChannelChip(channel: SpeakerChannel?) {
    val (container, content) = when (channel) {
        SpeakerChannel.Left -> MaterialTheme.colorScheme.primary to MaterialTheme.colorScheme.onPrimary
        SpeakerChannel.Right -> shareColors().container to shareColors().onContainer
        else -> Color.Transparent to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Text(
        when (channel) {
            SpeakerChannel.Left -> "L"
            SpeakerChannel.Right -> "R"
            else -> "Both"
        },
        style = MaterialTheme.typography.labelLarge,
        fontWeight = if (channel == SpeakerChannel.Left || channel == SpeakerChannel.Right) FontWeight.Bold else FontWeight.Normal,
        color = content,
        modifier = Modifier
            .clip(CircleShape)
            .background(container)
            .padding(horizontal = 12.dp, vertical = 6.dp),
    )
}

/** The file's picture: a note on an orange tint. */
@Composable
private fun Artwork(size: Int) {
    val colors = shareColors()
    Box(
        Modifier
            .size(size.dp)
            .clip(RoundedCornerShape((size / 4).dp))
            .background(colors.tint),
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Rounded.MusicNote, contentDescription = null, tint = colors.accent, modifier = Modifier.size((size / 2).dp))
    }
}

private fun time(seconds: Double): String {
    val total = seconds.toInt().coerceAtLeast(0)
    return "%d:%02d".format(total / 60, total % 60)
}
