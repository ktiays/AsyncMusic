//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.MaterialExpressiveTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MotionScheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/**
 * Material 3 Expressive with the system's dynamic colors for surfaces (Android 12+), but blue as
 * the accent everywhere, since blue means listening (and orange sharing) on every platform.
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun AsyncMusicTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val context = LocalContext.current
    val base = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        dark -> darkColorScheme()
        else -> lightColorScheme()
    }
    val colors = if (dark) {
        base.copy(
            primary = Color(0xFFA8C7FA),
            onPrimary = Color(0xFF062E6F),
            primaryContainer = Color(0xFF0842A0),
            onPrimaryContainer = Color(0xFFD3E3FD),
            inversePrimary = Color(0xFF0B57D0),
            surfaceTint = Color(0xFFA8C7FA),
        )
    } else {
        base.copy(
            primary = Color(0xFF0B57D0),
            onPrimary = Color.White,
            primaryContainer = Color(0xFFD3E3FD),
            onPrimaryContainer = Color(0xFF041E49),
            inversePrimary = Color(0xFFA8C7FA),
            surfaceTint = Color(0xFF0B57D0),
        )
    }
    MaterialExpressiveTheme(colorScheme = colors, motionScheme = MotionScheme.expressive(), content = content)
}

/**
 * The sharing accent, as orange as the other platforms' while readable on a button: a deep
 * orange with white text in light mode, a light one with dark text in dark mode.
 */
data class ShareColors(val accent: Color, val container: Color, val onContainer: Color, val tint: Color)

@Composable
fun shareColors(): ShareColors = if (isSystemInDarkTheme()) {
    ShareColors(accent = Color(0xFFFFB066), container = Color(0xFFFFB066), onContainer = Color(0xFF4A2500), tint = Color(0x33FF9F0A))
} else {
    ShareColors(accent = Color(0xFFC25400), container = Color(0xFFC25400), onContainer = Color.White, tint = Color(0x26FF9500))
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
