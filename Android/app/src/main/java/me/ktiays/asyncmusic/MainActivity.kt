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
import me.ktiays.asyncmusic.engine.AlertBehavior
import me.ktiays.asyncmusic.engine.GroupController
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
        if (savedInstanceState == null) {
            openFile(intent)
            // Share alerts reach a phone in the background as notifications: ask once, up front,
            // while the Listen screen explains them.
            if (!controller.settings.askedForNotifications && controller.settings.alertBehavior == AlertBehavior.ShowAlert) requestNotifications()
        }
    }

    override fun onStart() {
        super.onStart()
        controller.setAppVisible(true)
    }

    override fun onStop() {
        controller.setAppVisible(false)
        super.onStop()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        openFile(intent)
    }

    /** "Open with": share that file. */
    private fun openFile(intent: Intent?) {
        val uri = intent?.takeIf { it.action == Intent.ACTION_VIEW }?.data ?: return
        controller.openFile(uri)
    }

    /**
     * Share alerts and the notification while listening or sharing need permission from
     * Android 13 on. Asked when the app first opens, and again when joining or sharing if it
     * wasn't given (the system stops showing the request after it's been declined twice).
     */
    private fun requestNotifications() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
        controller.settings.askedForNotifications = true
        notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
    }
}
