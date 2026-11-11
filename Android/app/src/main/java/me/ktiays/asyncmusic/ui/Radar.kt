//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialShapes
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.ripple
import androidx.compose.material3.toShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.layout
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import me.ktiays.asyncmusic.engine.NearbyShare
import me.ktiays.asyncmusic.engine.ShareDeviceKind
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

private val RadarHeight = 350.dp
/** The center sits this far above the middle of the box, since labels hang below bubbles. */
private val CenterLift = 10.dp
private val OuterRing = 150.dp
private val InnerRing = 100.dp
private val HaloRadius = 52.dp
private val DeviceSize = 56.dp

/**
 * Where shares sit, newest first: pentagon corners (degrees, 0 = right, clockwise), how far out,
 * and how the labels line up under the bubble. Labels of the two upper slots run outward from the
 * bubble's outer edge, so they keep clear of the halo; the lower ones hang away from it anyway.
 */
private data class Slot(val angle: Float, val distance: Dp, val labels: LabelSide)

private enum class LabelSide { Center, TowardRight, TowardLeft }

private val Slots = listOf(
    Slot(-126f, 128.dp, LabelSide.TowardLeft),
    Slot(-54f, 128.dp, LabelSide.TowardRight),
    Slot(18f, 126.dp, LabelSide.Center),
    Slot(162f, 126.dp, LabelSide.Center),
    Slot(90f, 132.dp, LabelSide.Center),
)

/**
 * Shares nearby around this device: two faint rings, this device in a halo at the center, and up
 * to five shares as bubbles on the corners of a pentagon, the newest first so it always has a
 * place. Without shares, this device turns slowly and a ring pulses out of the halo.
 */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun Radar(
    shares: List<NearbyShare>,
    deviceKind: ShareDeviceKind,
    joinedShare: Long?,
    onShare: (NearbyShare) -> Unit,
    modifier: Modifier = Modifier,
) {
    val isEmpty = shares.isEmpty()
    val onSurface = MaterialTheme.colorScheme.onSurface
    val primary = MaterialTheme.colorScheme.primary
    val transition = rememberInfiniteTransition(label = "radar")
    val pulse by transition.animateFloat(0f, 1f, infiniteRepeatable(tween(2400, easing = LinearEasing), RepeatMode.Restart), label = "pulse")
    val turn by transition.animateFloat(0f, 360f, infiniteRepeatable(tween(20_000, easing = LinearEasing)), label = "turn")

    Box(
        modifier
            .fillMaxWidth()
            .height(RadarHeight),
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val center = Offset(size.width / 2, size.height / 2 - CenterLift.toPx())
            val hairline = 1.dp.toPx()
            drawCircle(onSurface.copy(alpha = 0.08f), radius = OuterRing.toPx(), center = center, style = Stroke(hairline))
            drawCircle(onSurface.copy(alpha = 0.12f), radius = InnerRing.toPx(), center = center, style = Stroke(hairline))
            if (isEmpty) {
                // One ring growing out of the halo (scale 1 to 2.9) and fading, again and again.
                drawCircle(
                    primary.copy(alpha = 0.9f * (1 - pulse)),
                    radius = HaloRadius.toPx() * (1 + 1.9f * pulse),
                    center = center,
                    style = Stroke(1.5.dp.toPx()),
                )
            }
            drawCircle(primary.copy(alpha = 0.10f), radius = HaloRadius.toPx(), center = center)
            drawCircle(primary.copy(alpha = 0.30f), radius = HaloRadius.toPx(), center = center, style = Stroke(hairline))
        }

        // This device.
        Box(
            Modifier
                .centeredAt(0.dp, 0.dp, DeviceSize)
                .size(DeviceSize)
                .semantics { contentDescription = "This device" },
            contentAlignment = Alignment.Center,
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .graphicsLayer { rotationZ = if (isEmpty) turn else 0f }
                    .clip(MaterialShapes.Cookie9Sided.toShape())
                    .background(primary),
            )
            Icon(deviceKind.icon(), contentDescription = null, tint = MaterialTheme.colorScheme.onPrimary, modifier = Modifier.size(26.dp))
        }

        shares.reversed().take(Slots.size).forEachIndexed { index, share ->
            key(share.id) {
                val slot = Slots[index]
                val radians = Math.toRadians(slot.angle.toDouble())
                ShareBubble(
                    share,
                    isNewest = index == 0,
                    isJoined = share.id == joinedShare,
                    dx = slot.distance * cos(radians).toFloat(),
                    dy = slot.distance * sin(radians).toFloat(),
                    labels = slot.labels,
                    onClick = { onShare(share) },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun ShareBubble(share: NearbyShare, isNewest: Boolean, isJoined: Boolean, dx: Dp, dy: Dp, labels: LabelSide, onClick: () -> Unit) {
    val spatial = MaterialTheme.motionScheme.defaultSpatialSpec<Float>()
    // Moves to a new place with a spring when the list changes, and springs in when it appears.
    val x by animateFloatAsState(dx.value, spatial, label = "x")
    val y by animateFloatAsState(dy.value, spatial, label = "y")
    val appear = remember { Animatable(0f) }
    LaunchedEffect(Unit) { appear.animateTo(1f, spatial) }
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val press by animateFloatAsState(if (pressed) 0.9f else 1f, MaterialTheme.motionScheme.fastSpatialSpec(), label = "press")

    val bubble = if (isNewest) 64.dp else 54.dp
    val primary = MaterialTheme.colorScheme.primary
    Column(
        Modifier
            .centeredAt(x.dp, y.dp, bubble, labels)
            .graphicsLayer {
                alpha = appear.value.coerceIn(0f, 1f)
                scaleX = appear.value
                scaleY = appear.value
            },
        horizontalAlignment = when (labels) {
            LabelSide.Center -> Alignment.CenterHorizontally
            LabelSide.TowardRight -> Alignment.Start
            LabelSide.TowardLeft -> Alignment.End
        },
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Box(
            Modifier
                .size(bubble)
                .graphicsLayer {
                    scaleX = press
                    scaleY = press
                }
                .clip(CircleShape)
                .background(if (isJoined) primary else MaterialTheme.colorScheme.surfaceContainerHigh)
                .border(if (isNewest) 2.dp else 1.dp, if (isNewest) primary else MaterialTheme.colorScheme.outlineVariant, CircleShape)
                .clickable(interactionSource = interaction, indication = ripple(), role = Role.Button, onClickLabel = "Join", onClick = onClick),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                share.deviceKind.icon(),
                contentDescription = null,
                tint = if (isJoined) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(if (isNewest) 26.dp else 22.dp),
            )
        }
        // On a pill of the background, so the rings never cross the text.
        Column(
            Modifier
                .widthIn(max = 120.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.surface)
                .padding(horizontal = 6.dp, vertical = 2.dp),
            horizontalAlignment = when (labels) {
                LabelSide.Center -> Alignment.CenterHorizontally
                LabelSide.TowardRight -> Alignment.Start
                LabelSide.TowardLeft -> Alignment.End
            },
        ) {
            Text(share.name, style = MaterialTheme.typography.labelLarge, fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis,
                textAlign = labels.textAlign)
            Text(
                share.source.ifEmpty { share.summary.substringBefore(" · ") },
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = labels.textAlign,
            )
        }
    }
}

private val LabelSide.textAlign: TextAlign
    get() = when (this) {
        LabelSide.Center -> TextAlign.Center
        LabelSide.TowardRight -> TextAlign.Start
        LabelSide.TowardLeft -> TextAlign.End
    }

/**
 * Places the element so that its [anchor]-sized bubble (top, and at the side [labels] says, with
 * the labels hanging below) is centered [dx], [dy] from the radar's center. It takes the whole
 * radar, so siblings overlap.
 */
private fun Modifier.centeredAt(dx: Dp, dy: Dp, anchor: Dp, labels: LabelSide = LabelSide.Center): Modifier = layout { measurable, constraints ->
    val placeable = measurable.measure(Constraints())
    layout(constraints.maxWidth, constraints.maxHeight) {
        val centerX = constraints.maxWidth / 2f + dx.toPx()
        val centerY = constraints.maxHeight / 2f - CenterLift.toPx() + dy.toPx()
        val half = anchor.toPx() / 2f
        val left = when (labels) {
            LabelSide.Center -> centerX - placeable.width / 2f
            LabelSide.TowardRight -> centerX - half
            LabelSide.TowardLeft -> centerX + half - placeable.width
        }
        placeable.place(left.roundToInt(), (centerY - half).roundToInt())
    }
}
