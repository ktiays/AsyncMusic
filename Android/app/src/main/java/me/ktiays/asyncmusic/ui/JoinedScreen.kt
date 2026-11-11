//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.animation.core.withInfiniteAnimationFrameMillis
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.VolumeDown
import androidx.compose.material.icons.automirrored.rounded.VolumeUp
import androidx.compose.material.icons.rounded.KeyboardArrowDown
import androidx.compose.material.icons.rounded.Pause
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.NearbyShare
import me.ktiays.asyncmusic.engine.ShareState
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout
import me.ktiays.asyncmusic.engine.SyncStatus
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

/** What the joined screen can do. */
interface JoinedActions {
    fun collapse()
    fun leave()
    fun setVolume(volume: Float)
    fun setPausedHere(paused: Boolean)
    /** In a stereo pair: play the left or the right side here. */
    fun requestChannel(channel: SpeakerChannel)
    fun join(share: NearbyShare)
}

/** The share listened to, full screen: what plays, which side, and this device's own controls. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun JoinedScreen(state: ShareState, share: NearbyShare, volume: Float, isPausedHere: Boolean, actions: JoinedActions) {
    Column(verticalArrangement = Arrangement.spacedBy(20.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = actions::collapse) {
                Icon(Icons.Rounded.KeyboardArrowDown, contentDescription = "Collapse")
            }
            Spacer(Modifier.weight(1f))
            SyncChip(state.syncStatus)
        }

        Surface(shape = RoundedCornerShape(36.dp), color = MaterialTheme.colorScheme.primaryContainer, modifier = Modifier.fillMaxWidth()) {
            Waveform(
                isPlaying = state.isPlaying && !isPausedHere,
                color = MaterialTheme.colorScheme.onPrimaryContainer,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(160.dp)
                    .padding(horizontal = 28.dp, vertical = 32.dp),
            )
        }

        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("LISTENING TO", style = MaterialTheme.typography.labelLargeEmphasized, color = MaterialTheme.colorScheme.primary)
            Text(share.name, style = MaterialTheme.typography.displaySmallEmphasized, maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(share.summary, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        if (state.joinedLayout == SpeakerLayout.StereoPair) {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(verticalAlignment = Alignment.Bottom) {
                    Text("Stereo pair", style = MaterialTheme.typography.titleMediumEmphasized, modifier = Modifier.weight(1f))
                    Text("Tap a side to play it", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    listOf(SpeakerChannel.Left, SpeakerChannel.Right).forEach { side ->
                        SideTile(side, state.channel, Modifier.weight(1f)) { actions.requestChannel(side) }
                    }
                }
            }
        } else {
            Surface(shape = RoundedCornerShape(24.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxWidth()) {
                PanelRow("Plays as") {
                    Text("Both channels", style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }

        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                FilledIconButton(
                    onClick = { actions.setPausedHere(!isPausedHere) },
                    shapes = IconButtonDefaults.shapes(),
                    modifier = Modifier.size(80.dp),
                ) {
                    Icon(
                        if (isPausedHere) Icons.Rounded.PlayArrow else Icons.Rounded.Pause,
                        contentDescription = if (isPausedHere) "Play here" else "Pause here",
                        modifier = Modifier.size(36.dp),
                    )
                }
                Icon(Icons.AutoMirrored.Rounded.VolumeDown, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Slider(
                    value = volume,
                    onValueChange = actions::setVolume,
                    modifier = Modifier
                        .weight(1f)
                        .semantics { contentDescription = "Volume" },
                )
                Icon(Icons.AutoMirrored.Rounded.VolumeUp, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (isPausedHere) {
                Text("Paused here · still in sync", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }

        Button(
            onClick = actions::leave,
            shapes = ButtonDefaults.shapesFor(ButtonDefaults.MediumContainerHeight),
            contentPadding = ButtonDefaults.contentPaddingFor(ButtonDefaults.MediumContainerHeight),
            colors = ButtonDefaults.buttonColors(
                containerColor = MaterialTheme.colorScheme.errorContainer,
                contentColor = MaterialTheme.colorScheme.onErrorContainer,
            ),
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = ButtonDefaults.MediumContainerHeight),
        ) {
            Text("Leave", style = ButtonDefaults.textStyleFor(ButtonDefaults.MediumContainerHeight))
        }

        val others = state.nearbyOthers
        if (others.isNotEmpty()) {
            NearbyList("Also sharing nearby", others.reversed(), joined = null, onJoin = actions::join, onOpen = {})
        }
    }
}

/** One side of a stereo pair: filled with the accent when this device plays it. */
@Composable
private fun SideTile(side: SpeakerChannel, playing: SpeakerChannel, modifier: Modifier, onClick: () -> Unit) {
    val isMine = side == playing
    val container = if (isMine) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceContainer
    val content = if (isMine) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurface
    Surface(shape = RoundedCornerShape(28.dp), color = container, modifier = modifier) {
        Column(
            Modifier
                .clickable(role = Role.RadioButton, onClick = onClick)
                .semantics { selected = isMine }
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(if (side == SpeakerChannel.Left) "L" else "R", style = MaterialTheme.typography.displayMedium, fontWeight = FontWeight.Bold, color = content)
            Text(
                when {
                    isMine -> "You"
                    playing == SpeakerChannel.Full -> "Tap to play"
                    else -> "Other speaker"
                },
                style = MaterialTheme.typography.titleMediumEmphasized,
                color = content,
            )
            Text(
                if (side == SpeakerChannel.Left) "Left channel" else "Right channel",
                style = MaterialTheme.typography.bodyMedium,
                color = content.copy(alpha = 0.75f),
            )
        }
    }
}

/** How the clock sync is going, as a chip: a green dot when in sync, amber while connecting. */
@Composable
fun SyncChip(status: SyncStatus) {
    val (text, tone) = when (status) {
        SyncStatus.Syncing -> "In sync" to Tone.Good
        SyncStatus.Reconnecting -> "Reconnecting…" to Tone.Pending
        SyncStatus.Failed -> "Can’t reach the sharer" to Tone.Bad
        SyncStatus.Connecting -> "Connecting…" to Tone.Pending
    }
    Surface(shape = CircleShape, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
        Row(Modifier.padding(horizontal = 14.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Box(
                Modifier
                    .size(8.dp)
                    .clip(CircleShape)
                    .background(tone.color()),
            )
            Text(text, style = MaterialTheme.typography.labelLarge)
        }
    }
}

/**
 * Bars that move while music plays, and rest, dimmed, when it doesn't. Not a level meter: the
 * motion only says that sound is coming.
 */
@Composable
fun Waveform(isPlaying: Boolean, color: Color, modifier: Modifier = Modifier, bars: Int = 28) {
    var time by remember { mutableLongStateOf(0L) }
    LaunchedEffect(isPlaying) {
        if (!isPlaying) return@LaunchedEffect
        while (true) withInfiniteAnimationFrameMillis { time = it }
    }
    Canvas(modifier) {
        val gap = size.width / bars * 0.4f
        val width = size.width / bars - gap
        for (index in 0 until bars) {
            val phase = index * 0.55f
            val level = if (isPlaying) {
                val t = time / 1000f
                0.25f + 0.75f * abs(sin(t * 2.1f + phase) * 0.6f + sin(t * 3.7f + phase * 1.7f) * 0.4f)
            } else {
                0.18f + 0.12f * abs(sin(phase * PI.toFloat() / 3))
            }
            val height = size.height * level.coerceIn(0.08f, 1f)
            drawRoundRect(
                color = if (isPlaying) color else color.copy(alpha = 0.4f),
                topLeft = Offset(index * (width + gap) + gap / 2, (size.height - height) / 2),
                size = Size(width, height),
                cornerRadius = CornerRadius(width / 2),
            )
        }
    }
}
