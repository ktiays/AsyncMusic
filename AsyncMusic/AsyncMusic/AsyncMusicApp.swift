//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI
#if os(macOS)
import SettingsKit
#endif

@main
struct AsyncMusicApp: App {
    #if os(macOS)
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    #endif

    var body: some Scene {
        #if os(macOS)
        // The Mac app lives in the menu bar; its panel is an AppKit window (`StatusItemController`).
        SettingsWindow(id: SettingsSectionID.windowID, groups: SettingsSectionID.groups) { section in
            SettingsContent(section: section, client: appDelegate.client, openDiagnostics: { appDelegate.openDiagnostics() })
        }
        .defaultLaunchBehavior(.suppressed)
        #else
        WindowGroup {
            ContentView()
        }
        #endif
    }
}

#if os(macOS)
/// The menu bar app: a remote control for AsyncMusic Server, which it starts on launch unless the
/// user turned it off.
final class AppDelegate: NSObject, NSApplicationDelegate {
    let client = ServiceClient()
    private var statusItem: StatusItemController?
    private let dockIcon = DockIconPolicy()

    func applicationDidFinishLaunching(_ notification: Notification) {
        dockIcon.start()
        client.start()
        let controller = StatusItemController(client: client)
        statusItem = controller
        // `-OpenPanelAtLaunch YES` opens the panel right away, for checking it while developing.
        if UserDefaults.standard.bool(forKey: "OpenPanelAtLaunch") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { controller.openPanel() }
        }
    }

    /// SwiftUI quits an app whose only scene is a `Window` when that window closes; this one
    /// keeps running in the menu bar.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func openDiagnostics() {
        statusItem?.openDiagnostics()
    }
}
#endif
