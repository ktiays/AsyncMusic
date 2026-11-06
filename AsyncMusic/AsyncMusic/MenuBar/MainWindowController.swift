//
//  Created by ktiays on 2026/11/06.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// The app's main window while it has no menu bar icon: an ordinary resizable window
/// (`MainWindowView`) that stays open until the user closes it and comes back from the Dock.
///
/// While it's the key window it tells the server the panel is open, since it lists new shares
/// the way the panel does.
final class MainWindowController: NSObject, NSWindowDelegate {
    let client: ServiceClient

    /// Made the first time it's shown.
    private var madeWindow: NSWindow?

    private static let defaultSize = NSSize(width: 780, height: 560)

    private var window: NSWindow {
        if let madeWindow { return madeWindow }
        let controller = NSHostingController(rootView: MainWindowView(client: client))
        controller.sizingOptions = [.minSize]
        // The view's title, subtitle and toolbar become the window's.
        controller.sceneBridgingOptions = [.title, .toolbars]
        let window = NSWindow(contentRect: NSRect(origin: .zero, size: Self.defaultSize),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: true)
        window.title = "AsyncMusic"
        window.toolbarStyle = .unified
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.contentViewController = controller
        window.setContentSize(Self.defaultSize)
        window.center()
        window.setFrameAutosaveName("Main Window")
        madeWindow = window
        return window
    }

    init(client: ServiceClient) {
        self.client = client
        super.init()
    }

    var isVisible: Bool { madeWindow?.isVisible ?? false }

    /// Brings the window up and makes it key.
    func show() {
        NSApp.activate()
        window.makeKeyAndOrderFront(nil)
    }

    /// Brings the window up behind the app's front window, such as Settings, without taking the
    /// user away from it.
    func showBehindFrontWindow() {
        let window = window
        let front = NSApp.orderedWindows.first { other in
            other !== window && other.isVisible && DockIconPolicy.showsDockIcon(for: other)
        }
        guard let front else { return show() }
        window.order(.below, relativeTo: front.windowNumber)
    }

    func close() {
        madeWindow?.close()
    }

    // MARK: - NSWindowDelegate

    func windowDidBecomeKey(_ notification: Notification) {
        client.setPanelVisible(true)
    }

    func windowDidResignKey(_ notification: Notification) {
        client.setPanelVisible(false)
    }
}
#endif
