//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// A borderless panel that stays put relative to `anchor` whatever size its content asks for:
/// hanging from a point (under the menu bar icon) or centered on one. AsyncMusic Server shows its
/// share alerts in one.
final class AnchoredPanel: NSPanel {
    enum Anchor {
        /// The top-left corner stays here; the panel grows down.
        case topLeft(NSPoint)
        /// The panel's center stays here; it grows both ways.
        case center(NSPoint)
    }

    /// The content's width; the window adds the shadow margin.
    static let contentWidth: CGFloat = 340

    /// Room around the content for the shadows it draws, which the window would otherwise clip.
    let margin: CGFloat = 14
    var width: CGFloat { Self.contentWidth + 2 * margin }

    var anchor: Anchor?
    var onCancel: (() -> Void)?
    private let allowsKey: Bool

    init(canBecomeKey: Bool) {
        allowsKey = canBecomeKey
        super.init(contentRect: NSRect(x: 0, y: 0, width: Self.contentWidth + 2 * margin, height: 120), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
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

    /// Takes the size the content asked for, keeping the panel at its anchor.
    func fit(_ size: CGSize) {
        let size = NSSize(width: ceil(size.width), height: ceil(size.height))
        guard size.width > 0, size.height > 0, size != frame.size else { return }
        setFrame(NSRect(origin: frame.origin, size: size), display: true)
    }

    /// Moves the panel to `anchor` at its current size.
    func place(at anchor: Anchor) {
        self.anchor = anchor
        setFrame(NSRect(origin: frame.origin, size: NSSize(width: width, height: max(frame.height, 40))), display: true)
    }

    /// Hosts `content` in this panel. The content reports its own height and the window follows
    /// it on the next turn of the run loop; resizing the window from inside SwiftUI's layout pass
    /// (what `sizingOptions` does) can recurse until the stack runs out.
    func host<Content: View>(@ViewBuilder _ content: () -> Content) {
        // The content hangs from the top of a root exactly the window's size, so the hosting view
        // doesn't center it in a window not yet at its height.
        let root = Color.clear.overlay(alignment: .top) {
            content()
                .padding(margin)
                .fixedSize(horizontal: false, vertical: true)
                .onGeometryChange(for: CGSize.self, of: { $0.size }) { [weak self] size in
                    DispatchQueue.main.async { self?.fit(size) }
                }
        }
        let controller = NSHostingController(rootView: root)
        controller.sizingOptions = []
        controller.view.wantsLayer = true
        controller.view.layer?.backgroundColor = .clear
        contentViewController = controller
    }

    /// Hanging from a menu bar icon at `iconFrame`, kept on its screen.
    func anchor(below iconFrame: NSRect) -> Anchor {
        let screen = NSScreen.screens.first { $0.frame.intersects(iconFrame) } ?? NSScreen.main
        let visible = screen?.visibleFrame ?? .zero
        var x = iconFrame.midX - width / 2
        x = min(max(x, visible.minX + 4 - margin), visible.maxX - width + margin - 4)
        return .topLeft(NSPoint(x: x, y: iconFrame.minY - 4 + margin))
    }

    override func cancelOperation(_ sender: Any?) {
        onCancel?()
    }

    private func anchored(_ rect: NSRect) -> NSRect {
        switch anchor {
        case let .topLeft(point):
            NSRect(x: point.x, y: point.y - rect.height, width: rect.width, height: rect.height)
        case let .center(point):
            NSRect(x: point.x - rect.width / 2, y: point.y - rect.height / 2, width: rect.width, height: rect.height)
        case nil:
            rect
        }
    }
}
#endif
