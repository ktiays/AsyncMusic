//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayCircle
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material.icons.filled.SpeakerGroup
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp

/** Shown while this device is not part of a group: host, or join someone else's group. */
@Composable
fun WelcomeScreen(deviceName: String, onHost: () -> Unit, onJoin: () -> Unit) {
    ReadableColumn(Modifier.verticalScroll(rememberScrollState())) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(top = 24.dp, bottom = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Icon(Icons.Filled.SpeakerGroup, contentDescription = null, Modifier.size(64.dp), tint = MaterialTheme.colorScheme.primary)
            Text("AsyncMusic", style = MaterialTheme.typography.headlineLarge, fontWeight = FontWeight.Bold)
            Text(
                "Play music in perfect sync across your devices — or split one song into true stereo over two of them.",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                textAlign = TextAlign.Center,
            )
        }
        SectionCard(onClick = onHost) {
            ChoiceRow(Icons.Filled.PlayCircle, "Host Music", "Play from this device. Nearby devices join as speakers.", showsChevron = true)
        }
        SectionCard(onClick = onJoin) {
            ChoiceRow(Icons.Filled.Speaker, "Join as Speaker", "Play along with a host on the same Wi-Fi.", showsChevron = true)
        }
        Text(
            "This device appears to others as “$deviceName”.",
            Modifier.fillMaxWidth(),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
    }
}
