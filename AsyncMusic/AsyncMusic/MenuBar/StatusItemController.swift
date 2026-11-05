//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import Observation
import SwiftUI

/// Owns the menu bar icon and the panel hanging from it, a borderless glass panel kept just
/// under the icon, its top edge fixed while SwiftUI changes its height.
///
/// Alerts for new shares are AsyncMusic Server's, so they also appear when the app isn't
/// running; the controller tells the server where the icon is, so they can point at it, and
/// when the panel is open, which lists new shares in their place.
final class StatusItemController: NSObject {
    let client: ServiceClient

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private lazy var panel: AnchoredPanel = {
        let panel = AnchoredPanel(canBecomeKey: true)
        panel.host {
            MenuBarPanel(client: client, willOpenSettings: { [unowned self] in
                closePanel()
                // Activate first, so the window SwiftUI opens next comes to the front.
                NSApp.activate()
            })
        }
        panel.onCancel = { [weak self] in self?.closePanel() }
        return panel
    }()
    private var outsideClickMonitor: Any?
    private var iconObservers: [NSObjectProtocol] = []

    init(client: ServiceClient) {
        self.client = client
        super.init()
        if let button = statusItem.button {
            button.target = self
            button.action = #selector(togglePanel)
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }
        observe()
        observeIconFrame()
    }

    // MARK: - Icon

    private func observe() {
        withObservationTracking {
            updateIcon()
        } onChange: { [weak self] in
            DispatchQueue.main.async { self?.observe() }
        }
    }

    private func updateIcon() {
        let state = client.state
        let icon: MenuBarIcon.State = state.isSharing ? .sharing : (state.joined != nil ? .playing : .listening)
        let badge = state.hasUnseenShares || !state.alerts.isEmpty
        statusItem.button?.image = MenuBarIcon.image(for: icon, badge: badge)
        // Dimmed while the server isn't running: nothing is listening.
        statusItem.button?.appearsDisabled = !client.isRunning
    }

    private var iconFrame: NSRect? {
        guard let button = statusItem.button, let window = button.window else { return nil }
        return window.convertToScreen(button.convert(button.bounds, to: nil))
    }

    /// Keeps the server told where the icon is, as the menu bar rearranges or changes screen.
    private func observeIconFrame() {
        let center = NotificationCenter.default
        let window = statusItem.button?.window
        for name in [NSWindow.didMoveNotification, NSWindow.didResizeNotification, NSWindow.didChangeScreenNotification] {
            iconObservers.append(center.addObserver(forName: name, object: window, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.reportIconFrame() }
            })
        }
        iconObservers.append(center.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reportIconFrame() }
        })
        // The status item's window is laid out after this turn of the run loop.
        DispatchQueue.main.async { [weak self] in self?.reportIconFrame() }
    }

    private func reportIconFrame() {
        client.setStatusItemFrame(statusItem.button?.window?.isVisible == true ? iconFrame : nil)
    }

    // MARK: - Panel

    @objc private func togglePanel() {
        if panel.isVisible {
            closePanel()
        } else {
            openPanel()
        }
    }

    func openPanel() {
        client.setPanelVisible(true)
        reportIconFrame()
        if let iconFrame { panel.place(at: .below(iconFrame)) }
        NSApp.activate()
        panel.makeKeyAndOrderFront(nil)
        statusItem.button?.highlight(true)
        installOutsideClickMonitor()
    }

    private func closePanel() {
        panel.orderOut(nil)
        statusItem.button?.highlight(false)
        client.setPanelVisible(false)
        removeOutsideClickMonitor()
    }

    private func installOutsideClickMonitor() {
        // `-KeepPanelOpen YES` leaves the panel open on outside clicks, for checking it while developing.
        guard outsideClickMonitor == nil, !UserDefaults.standard.bool(forKey: "KeepPanelOpen") else { return }
        outsideClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] _ in
            guard let self, panel.isVisible else { return }
            closePanel()
        }
    }

    private func removeOutsideClickMonitor() {
        guard let outsideClickMonitor else { return }
        NSEvent.removeMonitor(outsideClickMonitor)
        self.outsideClickMonitor = nil
    }

    func openDiagnostics() {
        closePanel()
        client.openDiagnostics()
    }
}
#endif
