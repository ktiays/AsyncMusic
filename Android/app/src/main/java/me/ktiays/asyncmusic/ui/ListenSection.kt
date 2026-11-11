//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.rounded.WarningAmber
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.NearbyShare
import me.ktiays.asyncmusic.engine.ShareDeviceKind
import me.ktiays.asyncmusic.engine.ShareState

/** What the Listen screen can do. */
interface ListenActions {
    fun join(share: NearbyShare)
    /** Shows the share joined, full screen. */
    fun openJoined()
}

/**
 * Listening, not looking at a joined share: the radar of shares nearby around this device and the
 * same shares as a list, or the radar alone, pulsing, while there are none.
 */
@Composable
fun ListenScreen(state: ShareState, deviceKind: ShareDeviceKind, actions: ListenActions) {
    Column(verticalArrangement = Arrangement.spacedBy(20.dp)) {
        state.discoveryError?.let { Notice(Icons.Rounded.WarningAmber, it) }
        state.joined?.let { NowListeningCard(it, actions::openJoined) }
        Radar(
            state.nearby,
            deviceKind,
            state.joined?.id,
            onShare = { share -> if (share.id == state.joined?.id) actions.openJoined() else actions.join(share) },
            // The labels of the bottom bubble hang a little below the radar's box.
            modifier = Modifier
                .align(Alignment.CenterHorizontally)
                .padding(bottom = 16.dp),
        )
        if (state.nearby.isEmpty()) {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Listening for shares", style = MaterialTheme.typography.headlineSmallEmphasized, textAlign = TextAlign.Center)
                Text(
                    "When a Mac, PC or phone on this network starts sharing audio, you’ll get an alert asking whether to join.",
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.padding(horizontal = 12.dp),
                )
            }
        } else {
            NearbyList(
                title = "${state.nearby.size} sharing nearby",
                shares = state.nearby.reversed(),
                joined = state.joined?.id,
                onJoin = actions::join,
                onOpen = actions::openJoined,
            )
        }
    }
}

/** The share being listened to, when its own screen is collapsed. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun NowListeningCard(share: NearbyShare, open: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(28.dp),
        color = MaterialTheme.colorScheme.primaryContainer,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            Modifier
                .clickable(onClickLabel = "Open", role = Role.Button, onClick = open)
                .padding(horizontal = 20.dp, vertical = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Waveform(isPlaying = true, color = MaterialTheme.colorScheme.onPrimaryContainer, modifier = Modifier.size(width = 40.dp, height = 28.dp), bars = 5)
            Column(Modifier.weight(1f)) {
                Text("LISTENING TO", style = MaterialTheme.typography.labelMediumEmphasized, color = MaterialTheme.colorScheme.onPrimaryContainer)
                Text(share.name, style = MaterialTheme.typography.titleLargeEmphasized, color = MaterialTheme.colorScheme.onPrimaryContainer, maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
            }
            Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = null, tint = MaterialTheme.colorScheme.onPrimaryContainer)
        }
    }
}

/**
 * Shares as rows in one rounded card: device tile, name, what it plays, and Join (filled for the
 * first, tonal for the rest); the share joined offers Open instead.
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun NearbyList(title: String, shares: List<NearbyShare>, joined: Long?, onJoin: (NearbyShare) -> Unit, onOpen: () -> Unit) {
    Surface(shape = RoundedCornerShape(28.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(vertical = 8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMediumEmphasized, modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp, bottom = 4.dp))
            shares.forEachIndexed { index, share ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    DeviceIcon(share.deviceKind, 48.dp)
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(share.name, style = MaterialTheme.typography.titleMediumEmphasized, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(share.summary, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1,
                            overflow = TextOverflow.Ellipsis)
                    }
                    when {
                        share.id == joined -> Button(onClick = onOpen, shapes = ButtonDefaults.shapes()) { Text("Open") }
                        index == 0 && joined == null -> Button(onClick = { onJoin(share) }, shapes = ButtonDefaults.shapes()) { Text("Join") }
                        else -> FilledTonalButton(onClick = { onJoin(share) }, shapes = ButtonDefaults.shapes()) { Text("Join") }
                    }
                }
            }
        }
    }
}
