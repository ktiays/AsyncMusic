//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.ui

import android.content.Context
import android.content.Intent
import android.provider.Settings as SystemSettings
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Notifications
import androidx.compose.material.icons.rounded.NotificationsOff
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3ExpressiveApi
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.core.app.NotificationManagerCompat
import androidx.lifecycle.compose.LifecycleResumeEffect
import me.ktiays.asyncmusic.engine.AlertBehavior
import me.ktiays.asyncmusic.engine.ShareDeviceKind

/** What the Settings screen can change. */
interface SettingsActions {
    fun rename(name: String)
    fun setAlertBehavior(behavior: AlertBehavior)
    /** Returns why it can't join, or null. */
    fun joinAddress(address: String): String?
    fun openDiagnostics()
}

@OptIn(ExperimentalMaterial3ExpressiveApi::class)
@Composable
fun SettingsScreen(
    customName: String,
    systemName: String,
    deviceKind: ShareDeviceKind,
    alertBehavior: AlertBehavior,
    actions: SettingsActions,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    ReadableColumn(modifier) {
        var name by rememberSaveable { mutableStateOf(customName) }
        PanelSection(if (deviceKind == ShareDeviceKind.Tablet) "This Tablet" else "This Phone") {
            item {
                OutlinedTextField(
                    value = name,
                    onValueChange = {
                        name = it
                        actions.rename(it)
                    },
                    label = { Text("Name") },
                    placeholder = { Text(systemName) },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                    supportingText = { Text("Other devices see this name when you share or listen.") },
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 8.dp),
                )
            }
        }

        var notificationsEnabled by remember { mutableStateOf(true) }
        LifecycleResumeEffect(Unit) {
            notificationsEnabled = NotificationManagerCompat.from(context).areNotificationsEnabled()
            onPauseOrDispose {}
        }
        PanelSection("Share Alerts") {
            item {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("When someone shares", style = MaterialTheme.typography.bodyLarge, modifier = Modifier.padding(horizontal = 4.dp))
                    ConnectedChoice(
                        AlertBehavior.entries,
                        alertBehavior,
                        label = { it.title },
                        onSelect = actions::setAlertBehavior,
                        icon = { if (it == AlertBehavior.ShowAlert) Icons.Rounded.Notifications else Icons.Rounded.NotificationsOff },
                    )
                    Text(
                        "“Only List” skips the alert; shares still appear in the Listen tab.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 4.dp),
                    )
                }
            }
            if (alertBehavior == AlertBehavior.ShowAlert && !notificationsEnabled) {
                item {
                    PanelRow(
                        "Notifications are off",
                        Modifier.clickable {
                            context.startActivity(
                                Intent(SystemSettings.ACTION_APP_NOTIFICATION_SETTINGS).putExtra(SystemSettings.EXTRA_APP_PACKAGE, context.packageName),
                            )
                        },
                    ) {
                        Text("Turn On", style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary)
                    }
                }
            }
        }

        var address by rememberSaveable { mutableStateOf("") }
        var addressError by rememberSaveable { mutableStateOf<String?>(null) }
        val join = {
            addressError = actions.joinAddress(address)
        }
        PanelSection("Join by Address") {
            item {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = address,
                        onValueChange = {
                            address = it
                            addressError = null
                        },
                        label = { Text("Address and port") },
                        placeholder = { Text("10.0.2.2:50000") },
                        singleLine = true,
                        isError = addressError != null,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Go),
                        keyboardActions = KeyboardActions(onGo = { join() }),
                        supportingText = {
                            Text(addressError ?: "For networks where shares can’t be heard, such as the Android emulator: the sharer’s address and clock port.")
                        },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Button(onClick = join, enabled = address.isNotBlank(), shapes = ButtonDefaults.shapes(), modifier = Modifier.align(Alignment.End)) {
                        Text("Join")
                    }
                }
            }
        }

        PanelSection("About") {
            item {
                PanelRow("Version") {
                    Text(appVersion(context), style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            item {
                PanelRow("Diagnostics", Modifier.clickable(onClick = actions::openDiagnostics)) {
                    Icon(Icons.AutoMirrored.Filled.KeyboardArrowRight, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

private fun appVersion(context: Context): String =
    runCatching { context.packageManager.getPackageInfo(context.packageName, 0).versionName }.getOrNull() ?: "1.0"
