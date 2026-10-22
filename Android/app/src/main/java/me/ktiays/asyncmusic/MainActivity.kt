//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import me.ktiays.asyncmusic.engine.GroupController
import me.ktiays.asyncmusic.engine.Role
import me.ktiays.asyncmusic.ui.AsyncMusicApp
import me.ktiays.asyncmusic.ui.AsyncMusicTheme

class MainActivity : ComponentActivity() {
    private val controller by lazy { GroupController.get(this) }
    private val notificationPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            AsyncMusicTheme {
                AsyncMusicApp(controller, ::requestNotifications)
            }
        }
        if (savedInstanceState == null) openFile(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        openFile(intent)
    }

    /** "Open with": host, with that file chosen. */
    private fun openFile(intent: Intent?) {
        val uri = intent?.takeIf { it.action == Intent.ACTION_VIEW }?.data ?: return
        if (controller.role.value == Role.Follower) return
        if (controller.role.value == Role.Off) {
            requestNotifications()
            controller.setRole(Role.Leader)
        }
        controller.loadTrack(uri)
    }

    /** The group's notification needs permission from Android 13 on; ask once, when joining a group. */
    private fun requestNotifications() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
        notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
}
