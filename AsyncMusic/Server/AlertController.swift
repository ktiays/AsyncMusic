//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AppKit
import Observation
import SwiftUI

/// Shows the alert for new shares. While the menu bar app shows its icon, it's a system popover
/// pointing at the icon; without the icon, a larger window in the middle of the screen in use,
/// with a close button. Either closes by itself after a while, unless the pointer rests on it.
final class AlertController: NSObject, NSPopoverDelegate {
    let center: ShareCenter

    /// The app's menu bar icon in screen coordinates, or nil when no icon is shown.
    var statusItemFrame: CGRect? {
        didSet {
            guard statusItemFrame != oldValue else { return }
            switch presentation {
            case .popover where statusItemFrame != nil:
                placeAnchor()
                popover.positioningRect = anchor.contentView?.bounds ?? .zero
            case .popover, .window:
                // The icon came or went: the alert takes the other form.
                close()
                present()
            case nil:
                break
            }
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

    private enum Presentation {
        case popover
        case window
    }

    private var presentation: Presentation?
    private let popover = NSPopover()
    /// An invisible window over the menu bar icon, for the popover to point at.
    private let anchor = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 1, height: 1), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
    private lazy var window = AlertWindow(cornerRadius: ShareAlertWindowView.cornerRadius) { [unowned self] in
        CenteredAlerts(center: center) { [weak self] hovering in self?.hoverChanged(hovering) }
    }
    private var outsideClickMonitor: Any?
    private var timeout: Task<Void, Never>?
    private var isHovered = false
    private var isClosingPopover = false

    private static let popoverDuration: Duration = .seconds(8)
    /// The window is bigger and away from where the user looks for status: a little longer.
    private static let windowDuration: Duration = .seconds(12)

    init(center: ShareCenter) {
        self.center = center
        super.init()
        anchor.backgroundColor = .clear
        anchor.isOpaque = false
        anchor.hasShadow = false
        anchor.ignoresMouseEvents = true
        anchor.hidesOnDeactivate = false
        anchor.level = .statusBar
        anchor.collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary, .transient, .ignoresCycle]

        popover.behavior = .applicationDefined
        popover.animates = true
        popover.delegate = self
        let controller = NSHostingController(rootView: ShareAlertsView(center: center) { [weak self] hovering in
            self?.hoverChanged(hovering)
        })
        controller.sizingOptions = .preferredContentSize
        popover.contentViewController = controller

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
        scheduleTimeout()
        installOutsideClickMonitor()
        if presentation == nil { present() }
    }

    /// Puts the alert on screen in the form that fits: popover under the icon, or window.
    private func present() {
        if statusItemFrame != nil {
            presentation = .popover
            placeAnchor()
            anchor.orderFrontRegardless()
            guard let view = anchor.contentView else { return }
            popover.show(relativeTo: view.bounds, of: view, preferredEdge: .minY)
        } else {
            presentation = .window
            window.present()
        }
    }

    /// Takes the alert off screen, leaving the shares alone.
    private func close() {
        switch presentation {
        case .popover:
            if popover.isShown {
                isClosingPopover = true
                popover.close()
            }
        case .window: window.dismiss()
        case nil: break
        }
        presentation = nil
        isHovered = false
    }

    private func hide() {
        timeout?.cancel()
        timeout = nil
        removeOutsideClickMonitor()
        close()
    }

    func popoverDidClose(_ notification: Notification) {
        if isClosingPopover {
            isClosingPopover = false
            if presentation != .popover { anchor.orderOut(nil) }
            return
        }
        // Closed by AppKit rather than through `close()`: the shares are ignored all the same.
        presentation = nil
        anchor.orderOut(nil)
        center.dismissAlerts()
    }

    /// Closes the alert after a while, counted from now or from when the pointer leaves it.
    private func scheduleTimeout() {
        timeout?.cancel()
        timeout = nil
        guard !isHovered else { return }
        let duration = statusItemFrame != nil ? Self.popoverDuration : Self.windowDuration
        timeout = Task { [weak self] in
            try? await Task.sleep(for: duration)
            guard !Task.isCancelled else { return }
            self?.center.dismissAlerts()
        }
    }

    private func hoverChanged(_ hovering: Bool) {
        isHovered = hovering
        guard presentation != nil else { return }
        scheduleTimeout()
    }

    private func placeAnchor() {
        guard let statusItemFrame else { return }
        anchor.setFrame(statusItemFrame, display: false)
    }

    private func installOutsideClickMonitor() {
        // `-KeepPanelOpen YES` leaves the alert open on outside clicks, for checking it while developing.
        guard outsideClickMonitor == nil, !UserDefaults.standard.bool(forKey: "KeepPanelOpen") else { return }
        outsideClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] _ in
            // The window in the middle of the screen has its own close button, and stays put
            // while the user goes on working around it.
            guard let self, presentation == .popover else { return }
            center.dismissAlerts()
        }
    }

    private func removeOutsideClickMonitor() {
        guard let outsideClickMonitor else { return }
        NSEvent.removeMonitor(outsideClickMonitor)
        self.outsideClickMonitor = nil
    }
}

/// The centered alert, kept up to date with the center's alerts.
private struct CenteredAlerts: View {
    let center: ShareCenter
    var onHover: (Bool) -> Void

    var body: some View {
        ShareAlertWindowView(alerts: center.alerts, join: { center.join($0) }, dismiss: { center.dismissAlerts() }, onHover: onHover)
    }
}
