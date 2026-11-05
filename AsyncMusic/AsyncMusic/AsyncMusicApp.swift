//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

@main
struct AsyncMusicApp: App {
    #if os(macOS)
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    #endif

    var body: some Scene {
        #if os(macOS)
        // The Mac app lives in the menu bar; its panel is an AppKit window (`StatusItemController`).
        Settings { EmptyView() }
        #else
        WindowGroup {
            ContentView()
        }
        #endif
    }
}

#if os(macOS)
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusItem: StatusItemController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let controller = StatusItemController(center: ShareCenter())
        statusItem = controller
        // `-OpenPanelAtLaunch YES` opens the panel right away, for checking it while developing.
        if UserDefaults.standard.bool(forKey: "OpenPanelAtLaunch") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { controller.openPanel() }
        }
    }
}
#endif
