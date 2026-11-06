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
///
/// With its menu bar icon (the default), its window is the panel hanging from the icon and the
/// app stays out of the Dock. With the icon turned off it's an ordinary app: a titled main
/// window that stays open until closed, and a Dock icon that brings it back.
final class AppDelegate: NSObject, NSApplicationDelegate {
    let client = ServiceClient()
    private let settings = AppSettings.shared
    private var statusItem: StatusItemController?
    private var mainWindow: MainWindowController?
    private let dockIcon = DockIconPolicy()

    func applicationDidFinishLaunching(_ notification: Notification) {
        dockIcon.start()
        client.start()
        let controller = StatusItemController(client: client)
        controller.onRemove = { [weak self] in self?.settings.showsMenuBarIcon = false }
        statusItem = controller
        mainWindow = MainWindowController(client: client)
        observePresentation(launching: true)
        // `-OpenPanelAtLaunch YES` opens the panel right away, for checking it while developing.
        if UserDefaults.standard.bool(forKey: "OpenPanelAtLaunch") {
            // Once the menu bar has connected the status item.
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { controller.openPanel() }
        }
    }

    private func observePresentation(launching: Bool) {
        withObservationTracking {
            applyPresentation(launching: launching)
        } onChange: { [weak self] in
            DispatchQueue.main.async { self?.observePresentation(launching: false) }
        }
    }

    private func applyPresentation(launching: Bool) {
        let showsIcon = settings.showsMenuBarIcon
        guard let statusItem, let mainWindow, launching || statusItem.isVisible != showsIcon else { return }
        statusItem.isVisible = showsIcon
        dockIcon.keepsDockIcon = !showsIcon
        if showsIcon {
            mainWindow.close()
        } else if launching {
            mainWindow.show()
        } else {
            // Turned off in Settings: the window takes the icon's place, behind Settings.
            mainWindow.showBehindFrontWindow()
        }
    }

    /// Opening the app again (from the Dock, Finder or Spotlight) shows its window.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if settings.showsMenuBarIcon {
            if !flag { statusItem?.openPanel() }
        } else if mainWindow?.isVisible != true {
            mainWindow?.show()
        }
        return false
    }

    /// SwiftUI quits an app whose only scene is a `Window` when that window closes; this one
    /// keeps running, in the menu bar or the Dock.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func openDiagnostics() {
        statusItem?.openDiagnostics()
    }
}
#endif
