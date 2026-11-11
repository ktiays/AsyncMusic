//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.DesktopMac
import androidx.compose.material.icons.rounded.DesktopWindows
import androidx.compose.material.icons.rounded.GraphicEq
import androidx.compose.material.icons.rounded.LaptopMac
import androidx.compose.material.icons.rounded.MusicNote
import androidx.compose.material.icons.rounded.Sensors
import androidx.compose.material.icons.rounded.Smartphone
import androidx.compose.material.icons.rounded.Speaker
import androidx.compose.material.icons.rounded.TabletAndroid
import androidx.compose.material3.ButtonGroupDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialShapes
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.ToggleButton
import androidx.compose.material3.ToggleButtonColors
import androidx.compose.material3.ToggleButtonDefaults
import androidx.compose.material3.toShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.graphicsLayer
import androidx.graphics.shapes.RoundedPolygon
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.ShareDeviceKind
import me.ktiays.asyncmusic.engine.ShareSourceKind
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SyncStatus

/** The content column of every page: centered, at most a readable width. */
@Composable
fun ReadableColumn(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Box(modifier.fillMaxWidth(), contentAlignment = Alignment.TopCenter) {
        Column(
            Modifier
                .widthIn(max = 600.dp)
                .fillMaxWidth()
                .padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
            content = content,
        )
    }
}

/** A rounded group of related content, with an optional title (diagnostics). */
@Composable
fun SectionCard(title: String? = null, modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Surface(shape = RoundedCornerShape(24.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = modifier.fillMaxWidth()) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (title != null) {
                Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            }
            content()
        }
    }
}

/** A label on the left, a value on the right. */
@Composable
fun LabeledRow(label: String, modifier: Modifier = Modifier, value: @Composable () -> Unit) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.width(12.dp).weight(1f))
        value()
    }
}

@Composable
fun LabeledRow(label: String, value: String, modifier: Modifier = Modifier) {
    LabeledRow(label, modifier) {
        Text(value, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.End)
    }
}

// MARK: - Grouped lists

/** Collects the items of a [PanelSection]. */
class PanelScope {
    internal val items = mutableListOf<@Composable () -> Unit>()

    fun item(content: @Composable () -> Unit) {
        items += content
    }
}

private val OuterCorner = 24.dp
private val InnerCorner = 6.dp

/** The shape of item [index] of [count] in a segmented group: round outside, tight inside. */
fun segmentShape(index: Int, count: Int): Shape {
    val top = if (index == 0) OuterCorner else InnerCorner
    val bottom = if (index == count - 1) OuterCorner else InnerCorner
    return RoundedCornerShape(topStart = top, topEnd = top, bottomStart = bottom, bottomEnd = bottom)
}

/**
 * A titled group of rows in the expressive style: each row its own tile, a hairline apart, the
 * group rounded on the outside.
 */
@Composable
fun PanelSection(title: String? = null, modifier: Modifier = Modifier, content: PanelScope.() -> Unit) {
    val items = PanelScope().apply(content).items
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (title != null) {
            Text(
                title,
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.padding(horizontal = 8.dp).semantics { heading() },
            )
        }
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            items.forEachIndexed { index, item ->
                Surface(shape = segmentShape(index, items.size), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxWidth()) {
                    item()
                }
            }
        }
    }
}

/** A label on the left and a control or value on the right, inside a [PanelSection]. */
@Composable
fun PanelRow(title: String, modifier: Modifier = Modifier, trailing: @Composable () -> Unit) {
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 60.dp)
            .padding(horizontal = 20.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
        Spacer(Modifier.width(12.dp))
        trailing()
    }
}

/**
 * A single choice among a few, as a connected button group: the selected button fills and rounds
 * fully, the others keep tight inner corners. [checkedColors] tints a choice of its own (the
 * orange of sharing).
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun <T> ConnectedChoice(
    options: List<T>,
    /** Null: none of them (yet). */
    selected: T?,
    label: (T) -> String,
    onSelect: (T) -> Unit,
    modifier: Modifier = Modifier,
    icon: ((T) -> ImageVector)? = null,
    checkedColors: @Composable (T) -> ToggleButtonColors = { ToggleButtonDefaults.toggleButtonColors() },
) {
    Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(ButtonGroupDefaults.ConnectedSpaceBetween)) {
        options.forEachIndexed { index, option ->
            ToggleButton(
                checked = option == selected,
                onCheckedChange = { onSelect(option) },
                shapes = when (index) {
                    0 -> ButtonGroupDefaults.connectedLeadingButtonShapes()
                    options.lastIndex -> ButtonGroupDefaults.connectedTrailingButtonShapes()
                    else -> ButtonGroupDefaults.connectedMiddleButtonShapes()
                },
                colors = checkedColors(option),
                modifier = Modifier
                    .weight(1f)
                    .heightIn(min = 48.dp)
                    .semantics { role = Role.RadioButton },
            ) {
                if (icon != null) {
                    Icon(icon(option), contentDescription = null, modifier = Modifier.size(ToggleButtonDefaults.IconSize))
                    Spacer(Modifier.size(ToggleButtonDefaults.IconSpacing))
                }
                Text(label(option), maxLines = 1)
            }
        }
    }
}

// MARK: - Pictures

/** A problem worth knowing about, in an orange box. */
@Composable
fun Notice(icon: ImageVector, text: String, modifier: Modifier = Modifier) {
    val colors = shareColors()
    Row(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(24.dp))
            .background(colors.tint)
            .padding(16.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Icon(icon, contentDescription = null, tint = colors.accent)
        Text(text, style = MaterialTheme.typography.bodyMedium)
    }
}

/**
 * An icon on a shape from the expressive shape library, which turns slowly while [spins] (music
 * playing, a share listening).
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun ShapeTile(
    polygon: RoundedPolygon,
    container: Color,
    size: Dp,
    modifier: Modifier = Modifier,
    spins: Boolean = false,
    content: @Composable () -> Unit,
) {
    val rotation by rememberInfiniteTransition(label = "tile").animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(tween(12_000, easing = LinearEasing)),
        label = "rotation",
    )
    val shape = polygon.toShape()
    Box(modifier.size(size), contentAlignment = Alignment.Center) {
        Box(
            Modifier
                .fillMaxSize()
                .graphicsLayer { rotationZ = if (spins) rotation else 0f }
                .clip(shape)
                .background(container),
        )
        content()
    }
}

/** A picture of the kind of device that shares. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun DeviceIcon(kind: ShareDeviceKind, size: Dp, modifier: Modifier = Modifier) {
    ShapeTile(MaterialShapes.Cookie12Sided, MaterialTheme.colorScheme.secondaryContainer, size, modifier) {
        Icon(kind.icon(), contentDescription = null, tint = MaterialTheme.colorScheme.onSecondaryContainer, modifier = Modifier.size(size * 0.5f))
    }
}

fun ShareDeviceKind.icon(): ImageVector = when (this) {
    ShareDeviceKind.Laptop -> Icons.Rounded.LaptopMac
    ShareDeviceKind.Desktop -> Icons.Rounded.DesktopMac
    ShareDeviceKind.Phone -> Icons.Rounded.Smartphone
    ShareDeviceKind.Tablet -> Icons.Rounded.TabletAndroid
    ShareDeviceKind.Unknown -> Icons.Rounded.Speaker
}
