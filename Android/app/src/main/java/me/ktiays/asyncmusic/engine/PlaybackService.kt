//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import me.ktiays.asyncmusic.MainActivity
import me.ktiays.asyncmusic.R

/**
 * A foreground service that runs while this device listens to a share or shares a file, so the
 * system keeps the process (and with it the clock sync and the music) going with the app in the
 * background or the screen off. Its notification says what the device is doing and offers to
 * leave or stop. It also takes the Join action of a share notification, which starts it.
 */
class PlaybackService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val controller = GroupController.get(this)
        when (intent?.action) {
            ACTION_STOP -> {
                val state = controller.shareState.value
                if (state.isSharing) controller.stopSharing() else controller.leave()
                return START_NOT_STICKY
            }
            ACTION_JOIN -> {
                // Started as a foreground service from the notification: show ours right away.
                isRunning = true
                createChannel(this)
                ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(this, controller.shareState.value, isJoining = true),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
                controller.join(intent.getLongExtra(EXTRA_SHARE, 0))
                return START_NOT_STICKY
            }
        }
        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(this, controller.shareState.value, isJoining = !controller.shareState.value.isActive),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        isRunning = false
        shownText = null
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_ID = "group"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_STOP = "me.ktiays.asyncmusic.STOP"
        private const val ACTION_JOIN = "me.ktiays.asyncmusic.JOIN"
        private const val EXTRA_SHARE = "share"
        @Volatile private var isRunning = false
        private var shownText: String? = null

        fun start(context: Context) {
            if (isRunning) return
            createChannel(context)
            isRunning = true
            ContextCompat.startForegroundService(context, Intent(context, PlaybackService::class.java))
        }

        fun stop(context: Context) {
            if (!isRunning) return
            isRunning = false
            shownText = null
            context.stopService(Intent(context, PlaybackService::class.java))
        }

        /** Keeps the notification's text in step with the state. */
        fun update(context: Context, state: ShareState, isJoining: Boolean) {
            if (!isRunning) return
            val text = content(state, isJoining).toString()
            if (text == shownText) return
            shownText = text
            context.getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(context, state, isJoining))
        }

        /** What a share notification's Join button does: start this service, which joins [share]. */
        fun joinIntent(context: Context, share: Long, requestCode: Int): PendingIntent =
            PendingIntent.getForegroundService(
                context,
                requestCode,
                Intent(context, PlaybackService::class.java).setAction(ACTION_JOIN).putExtra(EXTRA_SHARE, share),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
            )

        private fun createChannel(context: Context) {
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }

        private data class Content(val title: String, val text: String, val action: String?)

        private fun content(state: ShareState, isJoining: Boolean): Content {
            val joined = state.joined
            return when {
                state.isSharing -> {
                    val count = state.listeners.size
                    Content(
                        "Sharing ${state.source?.name ?: "audio"}",
                        state.shareProblem ?: if (count == 0) "No one has joined yet" else "$count listening",
                        "Stop Sharing",
                    )
                }
                joined != null -> Content(
                    "Listening to ${joined.name}",
                    when (state.syncStatus) {
                        SyncStatus.Syncing -> joined.summary
                        SyncStatus.Reconnecting -> "Reconnecting…"
                        SyncStatus.Failed -> "Can’t reach ${joined.name}"
                        SyncStatus.Connecting -> "Connecting…"
                    },
                    "Leave",
                )
                isJoining -> Content("Joining…", "AsyncMusic", null)
                else -> Content("AsyncMusic", "Idle", null)
            }
        }

        private fun notification(context: Context, state: ShareState, isJoining: Boolean): Notification {
            val content = content(state, isJoining)
            val open = PendingIntent.getActivity(context, 0, Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE)
            val builder = NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(content.title)
                .setContentText(content.text)
                .setContentIntent(open)
                .setOngoing(true)
                .setSilent(true)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            if (content.action != null) {
                val stop = PendingIntent.getService(context, 1, Intent(context, PlaybackService::class.java).setAction(ACTION_STOP),
                    PendingIntent.FLAG_IMMUTABLE)
                builder.addAction(0, content.action, stop)
            }
            return builder.build()
        }
    }
}
