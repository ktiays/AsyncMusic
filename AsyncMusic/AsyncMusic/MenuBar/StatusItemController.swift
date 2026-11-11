//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import Observation
import OSLog
import SwiftUI

/// Owns the menu bar icon and its menu: an `NSMenu` whose only item is the panel
/// (`MenuPanelItemView`), so opening, closing, tracking, keyboard and Space handling are the
/// system's own.
///
/// Alerts for new shares are AsyncMusic Server's, so they also appear when the app isn't
/// running; the controller tells the server where the icon is, so they can point at it, and
/// when the panel is open, which lists new shares in their place.
///
/// A right-click (or Control-click) on the icon opens a small menu instead, with Settings and Quit.
///
/// The icon can be hidden (`isVisible`), for an app that runs as an ordinary windowed app; the
/// user can also ⌘-drag it out of the menu bar, which `onRemove` reports.
final class StatusItemController: NSObject, NSMenuDelegate {
    let client: ServiceClient
    /// The user dragged the icon out of the menu bar.
    var onRemove: (() -> Void)?

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private lazy var panelView = MenuPanelItemView(width: Self.panelWidth) {
        MenuBarPanel(client: client, willOpenSettings: { [unowned self] in
            closePanel()
            // Activate first, so the window SwiftUI opens next comes to the front.
            NSApp.activate()
        })
        .environment(\.dismissMenuBarPanel, { [unowned self] in closePanel() })
        // A menu's controls look active, as the menu has the keyboard, though the app isn't.
        .environment(\.controlActiveState, .key)
    }
    private lazy var menu: NSMenu = {
        let menu = NSMenu()
        menu.autoenablesItems = false
        menu.delegate = self
        menu.addItem(panelItem)
        return menu
    }()
    private lazy var panelItem: NSMenuItem = {
        let item = NSMenuItem()
        item.view = panelView
        return item
    }()
    /// What the menu shows instead of the panel on a right-click.
    private lazy var contextItems: [NSMenuItem] = {
        let settings = NSMenuItem(title: "Settings…", action: #selector(openSettings), keyEquivalent: ",")
        settings.target = self
        let quit = NSMenuItem(title: "Quit AsyncMusic", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        quit.target = NSApp
        return [settings, .separator(), quit]
    }()
    private let log = Logger(subsystem: AsyncMusicService.appBundleIdentifier, category: "StatusItem")
    private static let panelWidth: CGFloat = 340
    private var isPanelOpen = false
    private var iconObservers: [NSObjectProtocol] = []
    private var visibilityObservation: NSKeyValueObservation?

    init(client: ServiceClient) {
        self.client = client
        super.init()
        statusItem.menu = menu
        statusItem.behavior = .removalAllowed
        observe()
        observeIconFrame()
        visibilityObservation = statusItem.observe(\.isVisible) { [weak self] item, _ in
            let isVisible = item.isVisible
            DispatchQueue.main.async { self?.visibilityChanged(isVisible) }
        }
    }

    /// Whether the app wants the icon shown; the icon going away while it does means the user
    /// dragged it out.
    private var wantsVisible = true

    var isVisible: Bool {
        get { wantsVisible }
        set {
            wantsVisible = newValue
            if !newValue { closePanel() }
            // AppKit saves the item's visibility and restores it at launch, so the item may start
            // out hidden whatever `wantsVisible` said.
            if statusItem.isVisible != newValue { statusItem.isVisible = newValue }
        }
    }

    private func visibilityChanged(_ isVisible: Bool) {
        // The icon's window is laid out after this turn of the run loop.
        DispatchQueue.main.async { [weak self] in self?.reportIconFrame() }
        guard !isVisible, wantsVisible else { return }
        wantsVisible = false
        closePanel()
        onRemove?()
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

    // MARK: - Menu

    /// Opens the menu as if the icon were clicked. From macOS 27 the menu bar opens it: the
    /// status item asks it to start navigating to the item
    /// (`-[NSSceneStatusItem _requestExpandedInterfaceSession]`, private), as a click would;
    /// a click on the button from code doesn't reach it.
    func openPanel() {
        guard wantsVisible, !isPanelOpen else { return }
        let request = NSSelectorFromString("_requestExpandedInterfaceSession")
        if statusItem.responds(to: request) {
            statusItem.perform(request)
        } else {
            statusItem.button?.performClick(nil)
        }
    }

    private func closePanel() {
        guard isPanelOpen else { return }
        menu.cancelTracking()
    }

    /// Fills the menu for the click opening it: the context menu for a right-click or
    /// Control-click, the panel otherwise.
    func menuNeedsUpdate(_ menu: NSMenu) {
        let event = NSApp.currentEvent
        let isContextClick = switch event?.type {
        case .rightMouseDown, .rightMouseUp: true
        case .leftMouseDown, .leftMouseUp: event?.modifierFlags.contains(.control) == true
        default: false
        }
        log.debug("Menu opening for event \(String(describing: event?.type), privacy: .public); context menu: \(isContextClick)")
        let items = isContextClick ? contextItems : [panelItem]
        guard menu.items != items else { return }
        menu.removeAllItems()
        for item in items { menu.addItem(item) }
    }

    func menuWillOpen(_ menu: NSMenu) {
        guard menu.items.first === panelItem else { return }
        isPanelOpen = true
        panelView.sizeToFitContent()
        client.setPanelVisible(true)
        reportIconFrame()
    }

    func menuDidClose(_ menu: NSMenu) {
        guard isPanelOpen else { return }
        isPanelOpen = false
        client.setPanelVisible(false)
    }

    /// Opens Settings through the app menu's Settings item, the SwiftUI scene's own command; AppKit
    /// has no other way into a SwiftUI window.
    @objc private func openSettings() {
        // Activate first, so the window comes to the front.
        NSApp.activate()
        guard let appMenu = NSApp.mainMenu?.items.first?.submenu,
              let index = appMenu.items.firstIndex(where: { $0.keyEquivalent == "," && $0.keyEquivalentModifierMask == .command }) else {
            log.error("No Settings item in the app menu")
            return
        }
        appMenu.performActionForItem(at: index)
        // macOS may not hand the app the focus (activation is up to the app in use), and the
        // window would then open behind that app's windows.
        DispatchQueue.main.async {
            let window = NSApp.windows.first { $0.isVisible && $0.identifier?.rawValue.hasPrefix(SettingsSectionID.windowID) == true }
            if let window, !window.isKeyWindow { window.orderFrontRegardless() }
        }
    }

    func openDiagnostics() {
        closePanel()
        client.openDiagnostics()
    }
}
#endif
