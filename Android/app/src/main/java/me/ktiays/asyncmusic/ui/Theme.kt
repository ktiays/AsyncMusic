//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import me.ktiays.asyncmusic.engine.SpeakerChannel

private val Indigo = Color(0xFF5856D6)

/** The system's dynamic colors where there are any (Android 12+), indigo otherwise. */
@Composable
fun AsyncMusicTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> darkColorScheme(primary = Color(0xFF8E8CF0), secondary = Color(0xFF8E8CF0))
        else -> lightColorScheme(primary = Indigo, secondary = Indigo)
    }
    MaterialTheme(colorScheme = colors, content = content)
}

/** Tones of a status dot. */
enum class Tone { Good, Pending, Bad, Neutral }

@Composable
fun Tone.color(): Color = when (this) {
    Tone.Good -> Color(0xFF34C759)
    Tone.Pending -> Color(0xFFFF9F0A)
    Tone.Bad -> MaterialTheme.colorScheme.error
    Tone.Neutral -> MaterialTheme.colorScheme.outline
}

/** A speaker badge's color: blue for left, orange for right (as on the other platforms). */
fun SpeakerChannel.tint(): Color? = when (this) {
    SpeakerChannel.Left -> Color(0xFF0A84FF)
    SpeakerChannel.Right -> Color(0xFFFF7A1A)
    SpeakerChannel.Full -> null
}
