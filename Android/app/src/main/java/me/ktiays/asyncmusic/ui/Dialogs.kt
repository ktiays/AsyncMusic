//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Hearing
import androidx.compose.material.icons.rounded.Sensors
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AlertDialogDefaults
import androidx.compose.material3.BasicAlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import me.ktiays.asyncmusic.engine.NearbyShare
import me.ktiays.asyncmusic.engine.ShareDeviceKind
import me.ktiays.asyncmusic.engine.ShareMode
import me.ktiays.asyncmusic.engine.ShareState

/** How long a share alert stays up unanswered, counted from the newest share in it. */
private const val ALERT_DURATION = 12_000L
private const val MAX_ALERT_ROWS = 4

/**
 * New shares, while the app is in front: one share large, several as a list, each with Join.
 * Closes by itself after a while; closing it (or waiting) just leaves the shares listed.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ShareAlertDialog(alerts: List<NearbyShare>, join: (NearbyShare) -> Unit, dismiss: () -> Unit) {
    LaunchedEffect(alerts.map { it.id }) {
        delay(ALERT_DURATION)
        dismiss()
    }
    BasicAlertDialog(onDismissRequest = dismiss) {
        Surface(shape = AlertDialogDefaults.shape, color = AlertDialogDefaults.containerColor, tonalElevation = AlertDialogDefaults.TonalElevation) {
            Column(Modifier.padding(24.dp)) {
                val single = alerts.singleOrNull()
                if (single != null) {
                    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                        DeviceIcon(single.deviceKind, 112.dp)
                        Spacer(Modifier.height(16.dp))
                        Text(single.name, style = MaterialTheme.typography.headlineSmallEmphasized, textAlign = TextAlign.Center, maxLines = 2,
                            overflow = TextOverflow.Ellipsis)
                        Spacer(Modifier.height(4.dp))
                        Text(
                            "Sharing ${single.summary}",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.Center,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                        Spacer(Modifier.height(24.dp))
                        Button(
                            onClick = { join(single) },
                            shapes = ButtonDefaults.shapesFor(ButtonDefaults.MediumContainerHeight),
                            contentPadding = ButtonDefaults.contentPaddingFor(ButtonDefaults.MediumContainerHeight),
                            modifier = Modifier
                                .fillMaxWidth()
                                .heightIn(min = ButtonDefaults.MediumContainerHeight),
                        ) {
                            Text("Join", style = ButtonDefaults.textStyleFor(ButtonDefaults.MediumContainerHeight))
                        }
                    }
                } else {
                    Text("${alerts.size} Shares Nearby", style = MaterialTheme.typography.headlineSmallEmphasized)
                    Spacer(Modifier.height(16.dp))
                    alerts.take(MAX_ALERT_ROWS).forEachIndexed { index, share ->
                        if (index > 0) HorizontalDivider(Modifier.padding(vertical = 10.dp), color = MaterialTheme.colorScheme.outlineVariant)
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            DeviceIcon(share.deviceKind, 48.dp)
                            Column(Modifier.weight(1f)) {
                                Text(share.name, style = MaterialTheme.typography.titleMediumEmphasized, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                Text("Sharing ${share.summary}", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    maxLines = 1, overflow = TextOverflow.Ellipsis)
                            }
                            Button(onClick = { join(share) }, shapes = ButtonDefaults.shapes()) { Text("Join") }
                        }
                    }
                    if (alerts.size > MAX_ALERT_ROWS) {
                        HorizontalDivider(Modifier.padding(vertical = 10.dp), color = MaterialTheme.colorScheme.outlineVariant)
                        Text("${alerts.size - MAX_ALERT_ROWS} more in the list", style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                Spacer(Modifier.height(8.dp))
                TextButton(onClick = dismiss, shapes = ButtonDefaults.shapes(), modifier = Modifier.align(Alignment.End)) { Text("Not Now") }
            }
        }
    }
}

/** What to ask before a mode switch that ends a share being listened to or given. */
data class ModeSwitchPrompt(val mode: ShareMode, val state: ShareState, val deviceKind: ShareDeviceKind) {
    private val device get() = if (deviceKind == ShareDeviceKind.Tablet) "tablet" else "phone"

    val title: String get() = if (mode == ShareMode.Share) "Start sharing from this $device?" else "Stop sharing?"

    val message: String
        get() {
            if (mode == ShareMode.Share) {
                return "You’ll stop listening to ${state.joined?.name ?: "the share"}. A $device either listens or shares, not both at once."
            }
            val count = state.listeners.size
            return if (count == 0) "Listening ends this share." else "$count ${if (count == 1) "device" else "devices"} listening to you will be disconnected."
        }

    val keepTitle: String get() = if (mode == ShareMode.Share) "Keep Listening" else "Keep Sharing"
    val switchTitle: String get() = if (mode == ShareMode.Share) "Leave and Share" else "Stop and Listen"
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ModeSwitchDialog(prompt: ModeSwitchPrompt, confirm: () -> Unit, dismiss: () -> Unit) {
    val accent = if (prompt.mode == ShareMode.Share) shareColors().accent else MaterialTheme.colorScheme.primary
    AlertDialog(
        onDismissRequest = dismiss,
        icon = { Icon(if (prompt.mode == ShareMode.Share) Icons.Rounded.Sensors else Icons.Rounded.Hearing, contentDescription = null, tint = accent) },
        title = { Text(prompt.title, textAlign = TextAlign.Center) },
        text = { Text(prompt.message, textAlign = TextAlign.Center) },
        confirmButton = { TextButton(onClick = confirm, shapes = ButtonDefaults.shapes()) { Text(prompt.switchTitle, color = accent) } },
        dismissButton = { TextButton(onClick = dismiss, shapes = ButtonDefaults.shapes()) { Text(prompt.keepTitle) } },
    )
}
