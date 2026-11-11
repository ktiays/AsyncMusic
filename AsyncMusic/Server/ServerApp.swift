//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AppKit
import SwiftUI
import os

/// AsyncMusic Server: the process that listens for shares, plays them, and shares this Mac's
/// audio. It runs as its own launchd job while the service is on, whether or not the menu bar
/// app is open, and shows its own alerts. The app is a remote control for it.
@main
enum ServerApp {
    private static let delegate = ServerDelegate()

    static func main() {
        if CommandLine.arguments.contains(AsyncMusicService.startArgument) {
            startService()
            exit(EXIT_SUCCESS)
        }
        let app = NSApplication.shared
        app.delegate = delegate
        app.setActivationPolicy(.accessory)
        app.run()
    }

    /// What the "Start at Login" item runs: loads the server's job, unless the user turned the
    /// service off, and exits. Exiting successfully either way keeps launchd from running it again.
    private static func startService() {
        let log = Logger(subsystem: AsyncMusicService.serverBundleIdentifier, category: "Service")
        let appDefaults = UserDefaults(suiteName: AsyncMusicService.appBundleIdentifier)
        guard appDefaults?.bool(forKey: AsyncMusicService.disabledKey) != true else {
            log.info("The service is off; not starting it at login")
            return
        }
        guard let executable = Bundle.main.executableURL else { return }
        do {
            try ServiceLauncher.start(executable: executable)
            log.info("Started the service at login")
        } catch {
            log.error("\(error.localizedDescription, privacy: .public)")
        }
    }
}

final class ServerDelegate: NSObject, NSApplicationDelegate {
    private let center = ShareCenter()
    private lazy var alerts = AlertController(center: center)
    private lazy var service = ServiceListener(center: center)
    private let dockIcon = DockIconPolicy()
    private var diagnosticsWindow: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        dockIcon.start()
        _ = alerts
        service.onCommand = { [weak self] command in self?.handle(command) }
        service.onAllClientsGone = { [weak self] in
            // The app quit: there's no icon to point at, and its panel is gone.
            self?.alerts.statusItemFrame = nil
            self?.alerts.isPanelVisible = false
        }
        service.resume()
    }

    /// It stays running after its windows close, like the menu bar app.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    private func handle(_ command: ServerCommand) {
        switch command {
        case let .updateSettings(settings): center.updateSettings(settings)
        case let .setStatusItemFrame(frame): alerts.statusItemFrame = frame
        case let .setPanelVisible(visible): alerts.isPanelVisible = visible
        case let .setMode(mode): center.setMode(mode)
        case let .join(id): center.join(id)
        case .leave: center.leave()
        case let .requestChannel(channel): center.requestChannel(channel)
        case .dismissAlerts: center.dismissAlerts()
        case let .setVolume(volume): center.setVolume(volume)
        case .refreshApplications: center.catalog.refresh()
        case let .selectSource(source): center.select(source)
        case .chooseFile: center.chooseFile()
        case .startSharing: center.startSharing()
        case .stopSharing: center.stopSharing()
        case let .setLayout(layout): center.setSpeakerLayout(layout.speakerLayout)
        case .openDiagnostics: openDiagnostics()
        }
    }

    private func openDiagnostics() {
        if diagnosticsWindow == nil {
            let window = NSWindow(contentViewController: NSHostingController(rootView: NavigationStack { DiagnosticsView(session: center.session) }))
            window.title = "AsyncMusic Diagnostics"
            window.setContentSize(NSSize(width: 520, height: 640))
            window.isReleasedWhenClosed = false
            diagnosticsWindow = window
        }
        NSApp.activate()
        diagnosticsWindow?.center()
        diagnosticsWindow?.makeKeyAndOrderFront(nil)
    }
}
