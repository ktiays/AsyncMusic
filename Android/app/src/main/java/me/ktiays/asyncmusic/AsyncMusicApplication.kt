//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

package me.ktiays.asyncmusic

import android.app.Application
import me.ktiays.asyncmusic.engine.GroupController

class AsyncMusicApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        // The engine lives as long as the process, independent of any activity.
        GroupController.get(this)
    }
}
