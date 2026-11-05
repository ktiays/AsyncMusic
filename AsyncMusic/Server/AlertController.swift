//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AppKit
import Observation
import SwiftUI

/// Shows the alert for new shares. While the menu bar app shows its icon, the alert grows out of
/// it, as a popover would; without the app, it appears in the middle of the screen in use.
final class AlertController {
    let center: ShareCenter

    /// The app's menu bar icon in screen coordinates, or nil when no icon is shown.
    var statusItemFrame: CGRect? {
        didSet {
            guard statusItemFrame != oldValue, panel.isVisible else { return }
            place()
        }
    }

    /// Whether the app's panel is open. It lists new shares itself, so no alert is needed.
    var isPanelVisible = false {
        didSet {
            guard isPanelVisible, !oldValue else { return }
            hide()
            center.panelOpened()
        }
    }

    private let panel = AnchoredPanel(canBecomeKey: false)
    /// What the alert view reads; it never touches the window itself.
    private let presentation = AlertPresentation()
    private var outsideClickMonitor: Any?
    private var timeout: Task<Void, Never>?

    private static let duration: TimeInterval = 30

    init(center: ShareCenter) {
        self.center = center
        panel.onCancel = { [weak self] in self?.center.dismissAlerts() }
        center.onAlert = { [weak self] in self?.show() }
        observe()
    }

    private func observe() {
        withObservationTracking {
            if center.alerts.isEmpty { hide() }
        } onChange: { [weak self] in
            DispatchQueue.main.async { self?.observe() }
        }
    }

    private func show() {
        guard !center.alerts.isEmpty else { return }
        if isPanelVisible {
            // The open panel lists the new share already.
            center.panelOpened()
            return
        }
        presentation.deadline = .now + Self.duration
        presentation.duration = Self.duration
        timeout?.cancel()
        timeout = Task { [weak self] in
            try? await Task.sleep(for: .seconds(Self.duration))
            guard !Task.isCancelled else { return }
            self?.center.dismissAlerts()
        }
        if !panel.isVisible {
            // A fresh view, so it grows out of its anchor again.
            panel.host { AlertHost(center: center, presentation: presentation) }
            place()
            panel.orderFrontRegardless()
        }
        installOutsideClickMonitor()
    }

    private func hide() {
        timeout?.cancel()
        timeout = nil
        guard panel.isVisible else { return }
        panel.orderOut(nil)
        removeOutsideClickMonitor()
    }

    private func place() {
        if let statusItemFrame {
            let anchor = panel.anchor(below: statusItemFrame)
            panel.place(at: anchor)
            presentation.anchorX = statusItemFrame.midX - panel.frame.minX - panel.margin
        } else {
            let mouse = NSEvent.mouseLocation
            let screen = NSScreen.screens.first { NSMouseInRect(mouse, $0.frame, false) } ?? NSScreen.main
            let visible = screen?.visibleFrame ?? .zero
            panel.place(at: .center(NSPoint(x: visible.midX, y: visible.midY)))
            presentation.anchorX = nil
        }
    }

    private func installOutsideClickMonitor() {
        // `-KeepPanelOpen YES` leaves the alert open on outside clicks, for checking it while developing.
        guard outsideClickMonitor == nil, !UserDefaults.standard.bool(forKey: "KeepPanelOpen") else { return }
        outsideClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] _ in
            guard let self, panel.isVisible else { return }
            center.dismissAlerts()
        }
    }

    private func removeOutsideClickMonitor() {
        guard let outsideClickMonitor else { return }
        NSEvent.removeMonitor(outsideClickMonitor)
        self.outsideClickMonitor = nil
    }
}

/// Where the alert's arrow points and when it closes by itself.
@Observable
final class AlertPresentation {
    /// The arrow's position from the alert's leading edge; nil for an alert without one.
    var anchorX: CGFloat?
    var deadline = Date.now
    var duration: TimeInterval = 30
}

private struct AlertHost: View {
    let center: ShareCenter
    let presentation: AlertPresentation

    var body: some View {
        ShareAlertsView(center: center, anchorX: presentation.anchorX, deadline: presentation.deadline, duration: presentation.duration)
    }
}
