//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import android.content.ClipData
import android.content.ClipboardManager
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.outlined.Analytics
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay
import me.ktiays.asyncmusic.engine.GroupController
import me.ktiays.asyncmusic.engine.Live
import me.ktiays.asyncmusic.engine.Role
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout

/**
 * The app's only window: the welcome screen, the host screen, the speaker screen (finding a
 * host, then what this device plays for it) and diagnostics. Which of the first three shows
 * follows the role asked for, so the screen survives the activity going away while the group
 * plays on.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AsyncMusicApp(controller: GroupController, requestNotifications: () -> Unit) {
    val context = LocalContext.current
    val state by controller.state.collectAsStateWithLifecycle()
    val role by controller.role.collectAsStateWithLifecycle()
    val leaders by controller.browser.leaders.collectAsStateWithLifecycle()
    val browseError by controller.browser.error.collectAsStateWithLifecycle()
    val isLoadingTrack by controller.isLoadingTrack.collectAsStateWithLifecycle()
    val loadError by controller.loadError.collectAsStateWithLifecycle()
    val advertisingError by controller.advertisingError.collectAsStateWithLifecycle()
    var showsDiagnostics by rememberSaveable { mutableStateOf(false) }

    // Render statistics, the output and the leader's clock change all the time; read them while
    // a screen shows them.
    val needsLive = showsDiagnostics || role != Role.Off
    val live by produceState(Live(), needsLive) {
        while (needsLive) {
            value = controller.live()
            delay(if (showsDiagnostics) 100 else 250)
        }
    }

    val chooseMusic = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) controller.loadTrack(uri)
    }

    BackHandler(showsDiagnostics) { showsDiagnostics = false }
    // Leaving the host list cancels the search; once in a group, back leaves the app playing.
    BackHandler(!showsDiagnostics && role == Role.Follower && state.connectedLeader == null) { controller.setRole(Role.Off) }

    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(
                title = {
                    Text(
                        when {
                            showsDiagnostics -> "Diagnostics"
                            role == Role.Leader -> "Hosting"
                            role == Role.Follower -> "Speaker"
                            else -> ""
                        },
                    )
                },
                navigationIcon = {
                    if (showsDiagnostics) {
                        IconButton(onClick = { showsDiagnostics = false }) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                        }
                    }
                },
                actions = {
                    if (!showsDiagnostics) {
                        IconButton(onClick = { showsDiagnostics = true }) {
                            Icon(Icons.Outlined.Analytics, contentDescription = "Diagnostics")
                        }
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background),
            )
        },
        containerColor = MaterialTheme.colorScheme.background,
    ) { padding ->
        Box(
            Modifier
                .fillMaxSize()
                .padding(padding),
        ) {
            when {
                showsDiagnostics -> DiagnosticsScreen(
                    state,
                    live,
                    leaders.size,
                    advertisingError,
                    controller.settings.customDeviceName.orEmpty(),
                    object : DiagnosticsActions {
                        override fun setManualOffset(milliseconds: Double) {
                            controller.manualOffset = milliseconds
                        }

                        override fun rename(name: String) = controller.rename(name)

                        override fun copyReport() {
                            val report = diagnosticsReport(context, controller.state.value, controller.live())
                            context.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("AsyncMusic diagnostics", report))
                        }

                        override fun clearLog() = controller.clearLog()
                    },
                )

                role == Role.Leader -> HostScreen(
                    state,
                    live,
                    isLoadingTrack,
                    loadError ?: advertisingError,
                    object : HostActions {
                        override fun chooseMusic() = chooseMusic.launch(arrayOf("audio/*"))
                        override fun play(source: Long) = controller.play(source)
                        override fun stopPlayback() = controller.stopPlayback()
                        override fun setLayout(layout: SpeakerLayout) = controller.setLayout(layout)
                        override fun assign(device: Long, channel: SpeakerChannel) = controller.assign(device, channel)
                        override fun swapChannels() = controller.swapChannels()
                        override fun stopHosting() = controller.setRole(Role.Off)
                    },
                )

                role == Role.Follower -> JoinScreen(
                    state,
                    leaders,
                    browseError,
                    live.output,
                    onFollow = controller::follow,
                    onFollowAddress = controller::follow,
                    onLeave = { controller.setRole(Role.Off) },
                )

                else -> WelcomeScreen(
                    state.identity.name,
                    onHost = {
                        requestNotifications()
                        controller.setRole(Role.Leader)
                    },
                    onJoin = {
                        requestNotifications()
                        controller.setRole(Role.Follower)
                    },
                )
            }
        }
    }
}
