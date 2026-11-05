//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit

/// Keeps an `LSUIElement` app out of the Dock except while one of its ordinary windows, such as
/// Settings or Diagnostics, is open, so the window can be found in the Dock and the app switcher.
final class DockIconPolicy {
    private var observers: [NSObjectProtocol] = []

    func start() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: NSWindow.didBecomeKeyNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.update(closing: nil) }
        })
        observers.append(center.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { [weak self] notification in
            let window = notification.object as? NSWindow
            MainActor.assumeIsolated { self?.update(closing: window) }
        })
    }

    private func update(closing: NSWindow?) {
        let hasWindow = NSApp.windows.contains { window in
            window !== closing && Self.showsDockIcon(for: window) && (window.isVisible || window.isMiniaturized)
        }
        let policy: NSApplication.ActivationPolicy = hasWindow ? .regular : .accessory
        guard NSApp.activationPolicy() != policy else { return }
        NSApp.setActivationPolicy(policy)
        if policy == .regular { NSApp.activate() }
    }

    /// Titled windows count; the menu bar panels and the status item's own window don't.
    static func showsDockIcon(for window: NSWindow) -> Bool {
        window.styleMask.contains(.titled) && !(window is NSPanel)
    }
}
#endif
