//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import me.ktiays.asyncmusic.MainActivity
import me.ktiays.asyncmusic.R

/**
 * The share alert while the app isn't in front: a notification per new share, with a Join button.
 * It goes away when the share ends, is joined, or the app comes to the front (where the share is
 * listed).
 */
object ShareNotifications {
    private const val CHANNEL_ID = "shares"
    private const val TAG = "share"
    private const val TIMEOUT = 15 * 60 * 1000L

    /** Shares with a notification up, by notification ID. */
    private val posted = mutableMapOf<Long, Int>()

    fun post(context: Context, share: NearbyShare) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, context.getString(R.string.share_notification_channel), NotificationManager.IMPORTANCE_HIGH).apply {
                description = context.getString(R.string.share_notification_channel_description)
            },
        )
        val id = (share.id xor (share.id ushr 32)).toInt()
        val open = PendingIntent.getActivity(
            context,
            id,
            Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(share.name)
            .setContentText("Sharing ${share.summary}")
            .setContentIntent(open)
            .setAutoCancel(true)
            // A share the app can't hear in the background may have ended long ago.
            .setTimeoutAfter(TIMEOUT)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_RECOMMENDATION)
            .addAction(0, "Join", PlaybackService.joinIntent(context, share.id, id))
            .build()
        manager.notify(TAG, id, notification)
        posted[share.id] = id
    }

    fun cancel(context: Context, share: Long) {
        val id = posted.remove(share) ?: return
        context.getSystemService(NotificationManager::class.java).cancel(TAG, id)
    }

    /** Takes down the notifications of shares other than [shares]. */
    fun retain(context: Context, shares: Set<Long>) {
        posted.keys.filter { it !in shares }.forEach { cancel(context, it) }
    }

    fun cancelAll(context: Context) {
        posted.keys.toList().forEach { cancel(context, it) }
    }
}
