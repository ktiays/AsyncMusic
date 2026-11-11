//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

/// The iPhone, iPad and Vision Pro app: the same Listen/Share model the Mac runs in AsyncMusic
/// Server, run here in the app. The system's media controls follow what plays, and new shares
/// arrive as notifications while the app plays in the background.
struct ContentView: View {
    @State private var center = ShareCenter()
    @State private var nowPlaying: NowPlayingController?
    @State private var notifications = ShareNotifications()
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        HomeView(center: center)
            .task {
                nowPlaying = NowPlayingController(center: center)
                notifications.attach(to: center)
                center.panelOpened()
            }
            .onChange(of: scenePhase) { _, phase in
                notifications.isAppActive = phase == .active
                // The app open is the Mac's open panel: shares are listed, not alerted.
                if phase == .active { center.panelOpened() }
            }
    }
}

#Preview {
    ContentView()
}
