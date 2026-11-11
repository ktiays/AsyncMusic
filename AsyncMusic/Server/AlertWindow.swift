//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AppKit
import SwiftUI

/// The share alert when there's no menu bar icon to point at: a glass window in the middle of the
/// screen in use. It never takes focus from the app in use, can be dragged anywhere by its
/// background, and keeps its center as the content grows or shrinks.
final class AlertWindow: NSPanel {
    private var center = NSPoint.zero
    /// Set while the window moves itself to fit its content.
    private var isFitting = false

    init<Content: View>(cornerRadius: CGFloat, @ViewBuilder content: () -> Content) {
        // Titled, with the title bar hidden, so the window server gives it a standard window's
        // shadow, edge and corners; a borderless window only gets a shadow traced from its pixels.
        super.init(contentRect: NSRect(x: 0, y: 0, width: 360, height: 200), styleMask: [.titled, .fullSizeContentView, .nonactivatingPanel], backing: .buffered, defer: true)
        titleVisibility = .hidden
        titlebarAppearsTransparent = true
        for button in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
            standardWindowButton(button)?.isHidden = true
        }
        isFloatingPanel = true
        level = .statusBar
        backgroundColor = .clear
        isOpaque = false
        hidesOnDeactivate = false
        isMovable = true
        isMovableByWindowBackground = true
        animationBehavior = .none
        collectionBehavior = [.moveToActiveSpace, .fullScreenAuxiliary, .transient, .ignoresCycle]

        // The window resizes itself on the next turn of the run loop; resizing it from inside
        // SwiftUI's layout pass (what `sizingOptions` does) can recurse.
        let hosting = NSHostingView(rootView: content()
            .fixedSize()
            .onGeometryChange(for: CGSize.self, of: { $0.size }) { [weak self] size in
                DispatchQueue.main.async { self?.fit(size) }
            })
        hosting.sizingOptions = []
        // The hidden title bar would push the content down.
        hosting.safeAreaRegions = []
        // Wherever it's dragged becomes the center it keeps while resizing.
        NotificationCenter.default.addObserver(forName: NSWindow.didMoveNotification, object: self, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, !self.isFitting else { return }
                self.center = NSPoint(x: self.frame.midX, y: self.frame.midY)
            }
        }
        let glass = NSGlassEffectView()
        glass.cornerRadius = cornerRadius
        glass.contentView = hosting
        contentView = glass
        setCornerRadius(cornerRadius)
    }

    /// Rounds the window itself (its edge, shadow and the window server's shape), which a titled
    /// window otherwise draws at the system's 16 points. Private AppKit, skipped if it's gone.
    private func setCornerRadius(_ radius: CGFloat) {
        let selector = NSSelectorFromString("_setCornerRadius:")
        guard let method = class_getInstanceMethod(NSWindow.self, selector),
              let types = method_getTypeEncoding(method), String(cString: types).hasPrefix("v24@0:8d") else { return }
        typealias Setter = @convention(c) (NSWindow, Selector, CGFloat) -> Void
        unsafeBitCast(method_getImplementation(method), to: Setter.self)(self, selector, radius)
    }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }

    /// Draws as a window of the active app although it never is one: Liquid Glass (the
    /// background and the close button) turns grey and flat in an inactive window, and the
    /// shadow lighter. Private AppKit; without it the window just looks inactive.
    @objc func _hasActiveAppearance() -> Bool { true }

    /// Fades in, centered on the screen the pointer is on.
    func present() {
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { NSMouseInRect(mouse, $0.frame, false) } ?? NSScreen.main
        let visible = screen?.visibleFrame ?? .zero
        center = NSPoint(x: visible.midX, y: visible.midY)
        fit(frame.size)
        alphaValue = 0
        orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.2
            animator().alphaValue = 1
        }
    }

    /// Fades out, then leaves the screen.
    func dismiss() {
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.2
            animator().alphaValue = 0
        } completionHandler: { [weak self] in
            guard let self, alphaValue == 0 else { return }
            orderOut(nil)
        }
    }

    private func fit(_ size: CGSize) {
        let size = NSSize(width: ceil(size.width), height: ceil(size.height))
        guard size.width > 0, size.height > 0 else { return }
        let rect = NSRect(x: round(center.x - size.width / 2), y: round(center.y - size.height / 2), width: size.width, height: size.height)
        guard rect != frame else { return }
        isFitting = true
        setFrame(rect, display: true, animate: isVisible && alphaValue > 0)
        isFitting = false
    }
}
