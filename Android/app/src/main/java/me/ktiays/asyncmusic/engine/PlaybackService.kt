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
 * A foreground service that runs while this device is part of a group, so the system keeps
 * the process (and with it the clock sync and the music) going with the app in the background
 * or the screen off. Its notification says what the device is doing and offers to leave.
 */
class PlaybackService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_LEAVE) {
            GroupController.get(this).setRole(Role.Off)
            return START_NOT_STICKY
        }
        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(this, GroupController.get(this).state.value),
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        isRunning = false
        super.onDestroy()
    }

    companion object {
        private const val CHANNEL_ID = "group"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_LEAVE = "me.ktiays.asyncmusic.LEAVE"
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

        /** Keeps the notification's text in step with the session. */
        fun update(context: Context, state: Snapshot) {
            if (!isRunning) return
            val text = description(state)
            if (text == shownText) return
            shownText = text
            context.getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, notification(context, state))
        }

        private fun createChannel(context: Context) {
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.notification_channel), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }

        private fun description(state: Snapshot): String = when (state.role) {
            Role.Leader -> {
                val devices = 1 + state.members.count { it.isActive }
                when {
                    state.isPlaying && devices > 1 -> "Playing on $devices devices"
                    state.isPlaying -> "Playing on this device"
                    else -> "Hosting · ${if (devices > 1) "$devices devices" else "waiting for speakers"}"
                }
            }
            Role.Follower -> state.connectedLeader?.let { "Speaker for $it" } ?: "Looking for hosts"
            Role.Off -> "Idle"
        }

        private fun notification(context: Context, state: Snapshot): Notification {
            val open = PendingIntent.getActivity(context, 0, Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
                PendingIntent.FLAG_IMMUTABLE)
            val leave = PendingIntent.getService(context, 1, Intent(context, PlaybackService::class.java).setAction(ACTION_LEAVE),
                PendingIntent.FLAG_IMMUTABLE)
            return NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(context.getString(R.string.app_name))
                .setContentText(description(state))
                .setContentIntent(open)
                .setOngoing(true)
                .setSilent(true)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
                .addAction(0, if (state.role == Role.Leader) "Stop Hosting" else "Leave", leave)
                .build()
        }
    }
}
