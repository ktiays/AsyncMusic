//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// A borderless panel that stays put relative to `anchor` whatever size its content asks for:
/// hanging from a point (under the menu bar icon) or centered on one.
final class AnchoredPanel: NSPanel {
    enum Anchor {
        /// The top-left corner stays here; the panel grows down.
        case topLeft(NSPoint)
        /// The panel's center stays here; it grows both ways.
        case center(NSPoint)
    }

    enum Background {
        /// A transparent window; the content draws its own shapes and shadows inside a margin.
        case none
        /// The window is a glass pane with rounded corners and the system's window shadow, and
        /// animates to each new size the content asks for.
        case glass(cornerRadius: CGFloat)
    }

    /// The content's width; a transparent window adds the shadow margin.
    static let contentWidth: CGFloat = 340

    /// Room around the content of a transparent window for the shadows it draws, which the
    /// window would otherwise clip. A glass window has the system's shadow and needs none.
    let margin: CGFloat
    var width: CGFloat { Self.contentWidth + 2 * margin }

    var anchor: Anchor?
    var onCancel: (() -> Void)?
    private let allowsKey: Bool
    private let glassView: NSGlassEffectView?
    private var hostingController: NSViewController?

    init(canBecomeKey: Bool, background: Background = .none) {
        allowsKey = canBecomeKey
        switch background {
        case .none:
            margin = 14
            glassView = nil
        case let .glass(cornerRadius):
            margin = 0
            let glass = NSGlassEffectView()
            glass.cornerRadius = cornerRadius
            glass.wantsLayer = true
            glass.layer?.cornerRadius = cornerRadius
            glass.layer?.cornerCurve = .continuous
            glass.layer?.masksToBounds = true
            glassView = glass
        }
        super.init(contentRect: NSRect(x: 0, y: 0, width: Self.contentWidth + 2 * margin, height: 120), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        isFloatingPanel = true
        level = .statusBar
        backgroundColor = .clear
        isOpaque = false
        hasShadow = glassView != nil
        hidesOnDeactivate = false
        isMovable = false
        animationBehavior = .utilityWindow
        acceptsMouseMovedEvents = true
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient, .ignoresCycle]
        if let glassView { contentView = glassView }
    }

    override var canBecomeKey: Bool { allowsKey }
    override var canBecomeMain: Bool { false }

    override func setFrame(_ frameRect: NSRect, display flag: Bool) {
        super.setFrame(anchored(frameRect), display: flag)
        // The shadow follows the window's shape, which changes with its size.
        if hasShadow { invalidateShadow() }
    }

    override func setFrame(_ frameRect: NSRect, display displayFlag: Bool, animate animateFlag: Bool) {
        super.setFrame(anchored(frameRect), display: displayFlag, animate: animateFlag)
        if hasShadow { invalidateShadow() }
    }

    /// Takes the size the content asked for, keeping the panel at its anchor. A glass window
    /// animates to it: the glass is the window, so it grows and shrinks with the frame, and the
    /// content is clipped to it on the way.
    func fit(_ size: CGSize) {
        let size = NSSize(width: ceil(size.width), height: ceil(size.height))
        guard size.width > 0, size.height > 0, size != frame.size else { return }
        let target = NSRect(origin: frame.origin, size: size)
        guard glassView != nil, isVisible else {
            stopResizing()
            setFrame(target, display: true)
            return
        }
        // Stepped by the display link rather than `animator()`: the animator proxy would call
        // the `setFrame` overrides with itself as `self`, reading `anchor` from the wrong object.
        let now = CACurrentMediaTime()
        // A new size mid-resize starts from where the window is, keeping its speed.
        let speed = resize?.heightSpeed(at: now) ?? 0
        let distance = size.height - frame.height
        resize = Resize(from: frame.size, to: size, start: now,
                        initialVelocity: abs(distance) < 1 ? 0 : speed / distance)
        if resizeLink == nil {
            let link = displayLink(target: self, selector: #selector(stepResize))
            link.add(to: .main, forMode: .common)
            resizeLink = link
        }
    }

    /// The spring the window resizes on. Content changing with the size should animate on it
    /// too (`.animation(.spring(AnchoredPanel.resizeSpring), value:)`), so the two move together.
    static let resizeSpring = Spring(duration: 0.3, bounce: 0.1)

    /// A resize from one size to another, as progress from 0 to 1 along `resizeSpring`.
    private struct Resize {
        var from: NSSize
        var to: NSSize
        var start: CFTimeInterval
        /// In progress per second.
        var initialVelocity: Double

        func progress(at time: CFTimeInterval) -> Double {
            AnchoredPanel.resizeSpring.value(target: 1.0, initialVelocity: initialVelocity, time: time - start)
        }

        func isSettled(at time: CFTimeInterval) -> Bool {
            time - start >= AnchoredPanel.resizeSpring.settlingDuration
        }

        /// In points per second.
        func heightSpeed(at time: CFTimeInterval) -> Double {
            let velocity = AnchoredPanel.resizeSpring.velocity(target: 1.0, initialVelocity: initialVelocity, time: time - start)
            return velocity * (to.height - from.height)
        }

        func size(at time: CFTimeInterval) -> NSSize {
            let p = progress(at: time)
            return NSSize(width: (from.width + (to.width - from.width) * p).rounded(),
                          height: (from.height + (to.height - from.height) * p).rounded())
        }
    }

    private var resize: Resize?
    private var resizeLink: CADisplayLink?

    @objc private func stepResize(_ link: CADisplayLink) {
        guard let resize else { return stopResizing() }
        let now = link.targetTimestamp
        let settled = resize.isSettled(at: now)
        setFrame(NSRect(origin: frame.origin, size: settled ? resize.to : resize.size(at: now)), display: true)
        if settled { stopResizing() }
    }

    private func stopResizing() {
        resize = nil
        resizeLink?.invalidate()
        resizeLink = nil
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
        // The content hangs from the top of a root exactly the window's size. As the root's
        // content it would make the root as tall as itself, and the hosting view would center
        // the root in a window still animating to that height, moving the content up and down.
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
        if let glassView {
            // While the window animates smaller, content taller than it stays inside the glass.
            controller.view.clipsToBounds = true
            hostingController = controller
            glassView.contentView = controller.view
        } else {
            contentViewController = controller
        }
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
