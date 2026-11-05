//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import Observation
import SwiftUI

/// Owns the menu bar icon and the two windows hanging from it: the panel (opened by clicking
/// the icon) and the share alert (opened by a new share). Both are borderless glass panels kept
/// just under the icon, their top edge fixed while SwiftUI changes their height.
final class StatusItemController: NSObject {
    let center: ShareCenter

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private lazy var panel = makePanel(canBecomeKey: true) { [unowned self] in
        MenuBarPanel(center: center, openDiagnostics: { [unowned self] in openDiagnostics() })
    }
    private lazy var alertPanel = makePanel(canBecomeKey: false) { [unowned self] in alertContent }
    private var hasAlertPanel = false
    /// What the alert view reads; it never touches the window itself.
    private let presentation = AlertPresentation()
    private var outsideClickMonitor: Any?
    private var diagnosticsWindow: NSWindow?
    private var alertTimeout: Task<Void, Never>?

    /// Room around the content for its shadow, which the window would otherwise clip.
    private static let margin: CGFloat = 14
    private static let alertDuration: TimeInterval = 30

    init(center: ShareCenter) {
        self.center = center
        super.init()
        if let button = statusItem.button {
            button.target = self
            button.action = #selector(togglePanel)
            button.sendAction(on: [.leftMouseUp, .rightMouseUp])
        }
        center.onAlert = { [weak self] in self?.showAlerts() }
        observe()
    }

    // MARK: - Icon

    private func observe() {
        withObservationTracking {
            updateIcon()
            if center.alerts.isEmpty { hideAlerts() }
        } onChange: { [weak self] in
            DispatchQueue.main.async { self?.observe() }
        }
    }

    private func updateIcon() {
        let state: MenuBarIcon.State = center.isSharing ? .sharing : (center.joined != nil ? .playing : .listening)
        let badge = center.hasUnseenShares || !center.alerts.isEmpty
        statusItem.button?.image = MenuBarIcon.image(for: state, badge: badge)
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
        hideAlerts()
        center.panelOpened()
        position(panel)
        NSApp.activate()
        panel.makeKeyAndOrderFront(nil)
        statusItem.button?.highlight(true)
        installOutsideClickMonitor()
    }

    private func closePanel() {
        panel.orderOut(nil)
        statusItem.button?.highlight(false)
        removeOutsideClickMonitorIfIdle()
    }

    // MARK: - Alerts

    private var alertContent: some View {
        AlertHost(center: center, presentation: presentation)
    }

    private func showAlerts() {
        guard !center.alerts.isEmpty else { return }
        if panel.isVisible {
            // The open panel lists the new share already.
            center.panelOpened()
            return
        }
        presentation.deadline = .now + Self.alertDuration
        presentation.duration = Self.alertDuration
        alertTimeout?.cancel()
        alertTimeout = Task { [weak self] in
            try? await Task.sleep(for: .seconds(Self.alertDuration))
            guard !Task.isCancelled else { return }
            self?.center.dismissAlerts()
        }
        if !alertPanel.isVisible {
            hasAlertPanel = true
            // A fresh view, so it grows out of the icon again.
            alertPanel.contentViewController = hostingController(for: alertPanel) { self.alertContent }
            position(alertPanel)
            if let iconFrame {
                presentation.anchorX = iconFrame.midX - alertPanel.frame.minX - Self.margin
            }
            alertPanel.orderFrontRegardless()
        }
        installOutsideClickMonitor()
    }

    private func hideAlerts() {
        alertTimeout?.cancel()
        alertTimeout = nil
        guard hasAlertPanel, alertPanel.isVisible else { return }
        alertPanel.orderOut(nil)
        removeOutsideClickMonitorIfIdle()
    }

    // MARK: - Windows

    private var iconFrame: NSRect? {
        guard let button = statusItem.button, let window = button.window else { return nil }
        return window.convertToScreen(button.convert(button.bounds, to: nil))
    }

    private func position(_ panel: AnchoredPanel) {
        guard let iconFrame else { return }
        let screen = statusItem.button?.window?.screen ?? NSScreen.main
        let visible = screen?.visibleFrame ?? .zero
        // Both windows are as wide as their content (340) plus the shadow margin.
        let width = 340 + 2 * Self.margin
        var x = iconFrame.midX - width / 2
        x = min(max(x, visible.minX + 4 - Self.margin), visible.maxX - width + Self.margin - 4)
        panel.anchor = NSPoint(x: x, y: iconFrame.minY - 4 + Self.margin)
        panel.setFrame(NSRect(x: x, y: panel.frame.minY, width: width, height: max(panel.frame.height, 40)), display: true)
    }

    private func makePanel<Content: View>(canBecomeKey: Bool, @ViewBuilder content: @escaping () -> Content) -> AnchoredPanel {
        let panel = AnchoredPanel(canBecomeKey: canBecomeKey)
        panel.contentViewController = hostingController(for: panel, content: content)
        panel.onCancel = { [weak self] in self?.closePanel() }
        return panel
    }

    /// Hosts `content` in `panel`. The content reports its own height and the window follows it
    /// on the next turn of the run loop; resizing the window from inside SwiftUI's layout pass
    /// (what `sizingOptions` does) can recurse until the stack runs out.
    private func hostingController<Content: View>(for panel: AnchoredPanel, @ViewBuilder content: () -> Content) -> NSHostingController<AnyView> {
        let root = content()
            .padding(Self.margin)
            .fixedSize(horizontal: false, vertical: true)
            .onGeometryChange(for: CGSize.self, of: { $0.size }) { [weak panel] size in
                DispatchQueue.main.async { panel?.fit(size) }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        let controller = NSHostingController(rootView: AnyView(root))
        controller.sizingOptions = []
        controller.view.wantsLayer = true
        controller.view.layer?.backgroundColor = .clear
        return controller
    }

    private func installOutsideClickMonitor() {
        // `-KeepPanelOpen YES` leaves the windows open on outside clicks, for checking them while developing.
        guard outsideClickMonitor == nil, !UserDefaults.standard.bool(forKey: "KeepPanelOpen") else { return }
        outsideClickMonitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] _ in
            guard let self else { return }
            if self.panel.isVisible { self.closePanel() }
            if self.hasAlertPanel, self.alertPanel.isVisible { self.center.dismissAlerts() }
        }
    }

    private func removeOutsideClickMonitorIfIdle() {
        guard !panel.isVisible, !(hasAlertPanel && alertPanel.isVisible), let monitor = outsideClickMonitor else { return }
        NSEvent.removeMonitor(monitor)
        outsideClickMonitor = nil
    }

    private func openDiagnostics() {
        closePanel()
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

/// Where the alert's arrow points and when it closes by itself.
@Observable
private final class AlertPresentation {
    var anchorX: CGFloat = 170
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

/// A borderless, transparent panel whose top-left corner stays at `anchor` whatever size its
/// content asks for.
final class AnchoredPanel: NSPanel {
    /// The top-left corner, in screen coordinates.
    var anchor: NSPoint?
    var onCancel: (() -> Void)?
    private let allowsKey: Bool

    init(canBecomeKey: Bool) {
        allowsKey = canBecomeKey
        super.init(contentRect: NSRect(x: 0, y: 0, width: 368, height: 120), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        isFloatingPanel = true
        level = .statusBar
        backgroundColor = .clear
        isOpaque = false
        hasShadow = false
        hidesOnDeactivate = false
        isMovable = false
        animationBehavior = .utilityWindow
        acceptsMouseMovedEvents = true
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient, .ignoresCycle]
    }

    override var canBecomeKey: Bool { allowsKey }
    override var canBecomeMain: Bool { false }

    override func setFrame(_ frameRect: NSRect, display flag: Bool) {
        super.setFrame(anchored(frameRect), display: flag)
    }

    override func setFrame(_ frameRect: NSRect, display displayFlag: Bool, animate animateFlag: Bool) {
        super.setFrame(anchored(frameRect), display: displayFlag, animate: animateFlag)
    }

    /// Takes the size the content asked for, keeping the top-left corner at `anchor`.
    func fit(_ size: CGSize) {
        let size = NSSize(width: ceil(size.width), height: ceil(size.height))
        guard size.width > 0, size.height > 0, size != frame.size else { return }
        setFrame(NSRect(origin: frame.origin, size: size), display: true)
    }

    override func cancelOperation(_ sender: Any?) {
        onCancel?()
    }

    private func anchored(_ rect: NSRect) -> NSRect {
        guard let anchor else { return rect }
        return NSRect(x: anchor.x, y: anchor.y - rect.height, width: rect.width, height: rect.height)
    }
}
#endif
