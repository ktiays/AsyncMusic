//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic.engine

import android.content.Context
import android.os.Build
import android.provider.Settings as SystemSettings
import androidx.core.content.edit
import java.security.SecureRandom

/** What the app keeps across launches. */
class Settings(private val context: Context) {
    private val preferences = context.getSharedPreferences("asyncmusic", Context.MODE_PRIVATE)

    /** This device's stable ID (created on first use) and the name others see. */
    val identity: DeviceIdentity
        get() {
            var id = preferences.getLong(DEVICE_ID, 0)
            if (id == 0L) {
                val random = SecureRandom()
                while (id == 0L) id = random.nextLong()
                preferences.edit { putLong(DEVICE_ID, id) }
            }
            return DeviceIdentity(id, customDeviceName ?: systemDeviceName)
        }

    /** The name set by the user, if any. Set an empty one to go back to the device's own. */
    var customDeviceName: String?
        get() = preferences.getString(DEVICE_NAME, null)?.takeIf { it.isNotBlank() }
        set(value) = preferences.edit { putString(DEVICE_NAME, value?.trim()) }

    /** The name the user gave the device in system settings ("Pixel 10 Pro"), or its model. */
    val systemDeviceName: String
        get() = SystemSettings.Global.getString(context.contentResolver, SystemSettings.Global.DEVICE_NAME)?.takeIf { it.isNotBlank() }
            ?: Build.MODEL

    var manualOffset: Double
        get() = Double.fromBits(preferences.getLong(MANUAL_OFFSET, 0.0.toBits()))
        set(value) = preferences.edit { putLong(MANUAL_OFFSET, value.toBits()) }

    var speakers: SpeakerSettings
        get() = SpeakerSettings(
            SpeakerLayout.of(preferences.getInt(SPEAKER_LAYOUT, 0)),
            preferences.getLong(LEFT_DEVICE, 0),
            preferences.getLong(RIGHT_DEVICE, 0),
        )
        set(value) = preferences.edit {
            putInt(SPEAKER_LAYOUT, value.layout.raw)
            putLong(LEFT_DEVICE, value.left)
            putLong(RIGHT_DEVICE, value.right)
        }

    private companion object {
        const val DEVICE_ID = "deviceID"
        const val DEVICE_NAME = "deviceName"
        const val MANUAL_OFFSET = "manualOffset"
        const val SPEAKER_LAYOUT = "speakerLayout"
        const val LEFT_DEVICE = "leftDevice"
        const val RIGHT_DEVICE = "rightDevice"
    }
}
