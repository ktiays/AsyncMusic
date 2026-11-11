//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.rounded.Headphones
import androidx.compose.material.icons.rounded.Sensors
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.FloatingToolbarDefaults
import androidx.compose.material3.HorizontalFloatingToolbar
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MediumFlexibleTopAppBar
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.ToggleButton
import androidx.compose.material3.ToggleButtonDefaults
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.delay
import me.ktiays.asyncmusic.engine.AlertBehavior
import me.ktiays.asyncmusic.engine.GroupController
import me.ktiays.asyncmusic.engine.Live
import me.ktiays.asyncmusic.engine.NearbyShare
import me.ktiays.asyncmusic.engine.ShareMode
import me.ktiays.asyncmusic.engine.SpeakerChannel
import me.ktiays.asyncmusic.engine.SpeakerLayout

private enum class Page { Main, Settings, Diagnostics }

/**
 * The app's only window: Listen (the radar, or the share joined) or Share, with the mode in a
 * floating toolbar at the bottom; Settings and Diagnostics. What it shows follows the share
 * center's state, so the screen survives the activity going away while a share plays on.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun AsyncMusicApp(controller: GroupController, requestNotifications: () -> Unit) {
    val state by controller.shareState.collectAsStateWithLifecycle()
    val deviceName by controller.deviceName.collectAsStateWithLifecycle()
    val volume by controller.volume.collectAsStateWithLifecycle()
    val isPausedHere by controller.isPausedHere.collectAsStateWithLifecycle()
    val sourceStatus by controller.sourceStatus.collectAsStateWithLifecycle()
    val alertBehavior by controller.alertBehavior.collectAsStateWithLifecycle()
    val modeRequest by controller.modeRequest.collectAsStateWithLifecycle()
    var page by rememberSaveable { mutableStateOf(Page.Main) }
    var pendingMode by rememberSaveable { mutableStateOf<ShareMode?>(null) }
    // The share joined shows full screen until collapsed; a new join shows it again.
    var collapsedShare by rememberSaveable { mutableStateOf<Long?>(null) }
    val joined = state.joined
    val showsJoined = page == Page.Main && state.mode == ShareMode.Listen && joined != null && collapsedShare != joined.id

    val chooseFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) controller.chooseFile(uri)
    }

    BackHandler(page != Page.Main) { page = if (page == Page.Diagnostics) Page.Settings else Page.Main }
    BackHandler(showsJoined) { collapsedShare = joined?.id }

    fun requestMode(mode: ShareMode) {
        if (state.switchInterrupts(mode)) pendingMode = mode else controller.setMode(mode)
    }

    fun join(share: NearbyShare) {
        requestNotifications()
        collapsedShare = null
        controller.join(share.id)
    }

    // Each page starts with the app bar expanded.
    val scrollBehavior = key(page) { TopAppBarDefaults.exitUntilCollapsedScrollBehavior() }
    Scaffold(
        modifier = if (page == Page.Main) Modifier else Modifier.nestedScroll(scrollBehavior.nestedScrollConnection),
        topBar = {
            if (page != Page.Main) {
                MediumFlexibleTopAppBar(
                    title = { Text(if (page == Page.Settings) "Settings" else "Diagnostics") },
                    navigationIcon = {
                        IconButton(onClick = { page = if (page == Page.Diagnostics) Page.Settings else Page.Main }) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                        }
                    },
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface,
                        scrolledContainerColor = MaterialTheme.colorScheme.surfaceContainer,
                    ),
                    scrollBehavior = scrollBehavior,
                )
            }
        },
        contentWindowInsets = if (page == Page.Main) WindowInsets(0) else TopAppBarDefaults.windowInsets,
        containerColor = MaterialTheme.colorScheme.surface,
    ) { padding ->
        Box(
            Modifier
                .fillMaxSize()
                .padding(padding),
        ) {
            when (page) {
                Page.Main -> {
                    AnimatedContent(
                        targetState = if (showsJoined) "joined" else state.mode.name + state.isSharing,
                        transitionSpec = { fadeIn() togetherWith fadeOut() },
                        label = "page",
                    ) { target ->
                        ReadableColumn(
                            Modifier
                                .verticalScroll(rememberScrollState())
                                .windowInsetsPadding(WindowInsets.safeDrawing),
                        ) {
                            when {
                                target == "joined" && joined != null -> JoinedScreen(
                                    state,
                                    joined,
                                    volume,
                                    isPausedHere,
                                    object : JoinedActions {
                                        override fun collapse() {
                                            collapsedShare = joined.id
                                        }

                                        override fun leave() = controller.leave()
                                        override fun setVolume(volume: Float) = controller.setVolume(volume)
                                        override fun setPausedHere(paused: Boolean) = controller.setPausedHere(paused)
                                        override fun requestChannel(channel: SpeakerChannel) = controller.requestChannel(channel)
                                        override fun join(share: NearbyShare) = join(share)
                                    },
                                )

                                state.mode == ShareMode.Listen -> {
                                    Header("Listening", "as $deviceName") { page = Page.Settings }
                                    ListenScreen(
                                        state,
                                        controller.deviceKind,
                                        object : ListenActions {
                                            override fun join(share: NearbyShare) = join(share)
                                            override fun openJoined() {
                                                collapsedShare = null
                                            }
                                        },
                                    )
                                }

                                else -> SharePage(controller, state, sourceStatus, deviceName, chooseFile = { chooseFile.launch(arrayOf("audio/*")) },
                                    requestNotifications = requestNotifications, openSettings = { page = Page.Settings })
                            }
                            // Room for the floating toolbar.
                            if (target != "joined") Spacer(Modifier.height(88.dp))
                        }
                    }
                    AnimatedVisibility(
                        visible = !showsJoined,
                        enter = slideInVertically { it } + fadeIn(),
                        exit = slideOutVertically { it } + fadeOut(),
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .navigationBarsPadding()
                            .padding(bottom = 16.dp),
                    ) {
                        ModeToolbar(state.mode, ::requestMode)
                    }
                }

                Page.Settings -> SettingsScreen(
                    modifier = Modifier.verticalScroll(rememberScrollState()),
                    customName = controller.settings.customDeviceName.orEmpty(),
                    systemName = controller.settings.systemDeviceName,
                    deviceKind = controller.deviceKind,
                    alertBehavior = alertBehavior,
                    actions = object : SettingsActions {
                        override fun rename(name: String) = controller.rename(name)
                        override fun setAlertBehavior(behavior: AlertBehavior) = controller.setAlertBehavior(behavior)
                        override fun joinAddress(address: String): String? {
                            requestNotifications()
                            val error = controller.joinAddress(address)
                            if (error == null) {
                                collapsedShare = null
                                page = Page.Main
                            }
                            return error
                        }

                        override fun openDiagnostics() {
                            page = Page.Diagnostics
                        }
                    },
                )

                Page.Diagnostics -> Diagnostics(controller)
            }
        }
    }

    // New shares while in front, as an alert that closes by itself.
    if (state.alerts.isNotEmpty() && state.mode == ShareMode.Listen) {
        ShareAlertDialog(
            state.alerts,
            join = { share ->
                join(share)
                page = Page.Main
            },
            dismiss = controller::dismissAlerts,
        )
    }

    val prompt = (pendingMode ?: modeRequest)?.let { ModeSwitchPrompt(it, state, controller.deviceKind) }
    if (prompt != null && state.switchInterrupts(prompt.mode)) {
        ModeSwitchDialog(
            prompt,
            confirm = {
                pendingMode = null
                controller.setMode(prompt.mode)
            },
            dismiss = {
                pendingMode = null
                controller.cancelModeRequest()
            },
        )
    }
    // A request whose connection has gone meanwhile (or that never cut one): just switch.
    LaunchedEffect(modeRequest, state.isActive) {
        val request = modeRequest
        if (request != null && !state.switchInterrupts(request)) controller.setMode(request)
    }
}

/** A screen's large title, a line under it, and the settings button. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun Header(title: String, subtitle: String, openSettings: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.Top) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.displaySmallEmphasized)
            Text(subtitle, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1,
                overflow = TextOverflow.Ellipsis)
        }
        IconButton(onClick = openSettings) {
            Icon(Icons.Outlined.Settings, contentDescription = "Settings")
        }
    }
}

/** Share mode: setting up, or the share under way. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun SharePage(
    controller: GroupController,
    state: me.ktiays.asyncmusic.engine.ShareState,
    sourceStatus: GroupController.SourceStatus,
    deviceName: String,
    chooseFile: () -> Unit,
    requestNotifications: () -> Unit,
    openSettings: () -> Unit,
) {
    val actions = object : ShareActions {
        override fun chooseFile() = chooseFile()
        override fun startSharing() {
            requestNotifications()
            controller.startSharing()
        }

        override fun stopSharing() = controller.stopSharing()
        override fun setLayout(layout: SpeakerLayout) = controller.setLayout(layout)
        override fun play() = controller.playForEveryone()
        override fun stop() = controller.stopForEveryone()
    }
    if (!state.isSharing) {
        Header("Share", "as $deviceName", openSettings)
        ShareSetup(state, sourceStatus, actions)
        return
    }
    val snapshot by controller.state.collectAsStateWithLifecycle()
    // The position changes all the time; read it while the share shows.
    val live by produceState(Live()) {
        while (true) {
            value = controller.live()
            delay(250)
        }
    }
    Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.Top) {
        SharingTitle(deviceName, Modifier.weight(1f))
        Button(
            onClick = actions::stopSharing,
            shapes = ButtonDefaults.shapes(),
            colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.errorContainer, contentColor = MaterialTheme.colorScheme.onErrorContainer),
        ) { Text("Stop") }
    }
    SharingLive(
        state,
        Progress(
            isPlaying = snapshot.isPlaying,
            elapsed = if (snapshot.isPlaying) live.stats.mediaSeconds else 0.0,
            duration = snapshot.selectedTrack?.duration ?: 0.0,
        ),
        actions,
    )
}

/** Listen or Share, floating at the bottom within thumb's reach. Share, selected, is orange. */
@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
private fun ModeToolbar(mode: ShareMode, select: (ShareMode) -> Unit) {
    val share = shareColors()
    HorizontalFloatingToolbar(
        expanded = true,
        colors = FloatingToolbarDefaults.standardFloatingToolbarColors(),
        modifier = Modifier.semantics { contentDescription = "Mode" },
    ) {
        ShareMode.entries.forEach { option ->
            val isShare = option == ShareMode.Share
            ToggleButton(
                checked = mode == option,
                onCheckedChange = { select(option) },
                colors = if (isShare) {
                    ToggleButtonDefaults.toggleButtonColors(checkedContainerColor = share.container, checkedContentColor = share.onContainer)
                } else {
                    ToggleButtonDefaults.toggleButtonColors()
                },
                modifier = Modifier.semantics { role = Role.RadioButton },
            ) {
                Icon(if (isShare) Icons.Rounded.Sensors else Icons.Rounded.Headphones, contentDescription = null, modifier = Modifier.size(ToggleButtonDefaults.IconSize))
                Spacer(Modifier.size(ToggleButtonDefaults.IconSpacing))
                Text(option.title)
            }
        }
    }
}

@Composable
private fun Diagnostics(controller: GroupController) {
    val context = LocalContext.current
    val snapshot by controller.state.collectAsStateWithLifecycle()
    val shares by controller.shareState.collectAsStateWithLifecycle()
    // Render statistics, the output and the leader's clock change all the time; read them while
    // this screen shows them.
    val live by produceState(Live()) {
        while (true) {
            value = controller.live()
            delay(100)
        }
    }
    DiagnosticsScreen(
        snapshot,
        live,
        shares.nearby.size,
        shares.discoveryError,
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
}
