//
//  Created by ktiays on 2026/11/06.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import ObjectiveC
import SwiftUI

/// The custom view of the status item menu's only item: SwiftUI content, with the menu following
/// its height.
///
/// The menu is AppKit's own (`NSPopupMenuWindow`), so its window, glass, opening and closing,
/// tracking and Space handling are the system's. When the content's height changes, the view
/// steps its own height there along the spring the content animates on, and on each step tells
/// the menu, the way the legacy custom-view path did
/// (`-[NSContextMenuImpl _menuItem:atIndex:customViewDidChangeSize:]`, which the menus of
/// macOS 27 no longer call on a frame change). The menu then measures the item and sets its
/// window to fit, laid out as for any other size change. Private; without it the menu keeps the
/// size it opened at.
///
/// AppKit's own animated resize (`-[NSPopupMenuWindow updateWindowFrameTo:offset:animated:completion:]`
/// with `animated`) isn't used: it lays the menu out for the old size before the window moves,
/// and runs on its own fixed timing.
final class MenuPanelItemView: NSView {
    /// The spring the content animates its changes on (`MenuBarPanel`), and the menu follows.
    static let spring = Spring(duration: 0.3, bounce: 0.2)
    private static let hostingHeight: CGFloat = 4000

    private let hostingView: NSView
    private let width: CGFloat
    /// The content's height; the view's own height follows it.
    private var contentHeight: CGFloat = 0
    private var resize: Resize?
    private var resizeLink: CADisplayLink?

    init<Content>(width: CGFloat, @ViewBuilder content: () -> Content) where Content: View {
        self.width = width
        // The content reports its height, taken on the next turn of the run loop, as resizing
        // from inside SwiftUI's layout pass can recurse.
        var updateHeight: (CGFloat) -> Void = { _ in }
        let root = content()
            .frame(width: width)
            .fixedSize(horizontal: false, vertical: true)
            .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { height in
                DispatchQueue.main.async { updateHeight(height) }
            }
            // Hangs from the top of a hosting view that never changes size: SwiftUI animates
            // the views that stay across a change from where they were laid out before, and a
            // hosting view resized meanwhile would move them.
            .frame(maxHeight: .infinity, alignment: .top)
        let hosting = NSHostingView(rootView: root)
        hosting.sizingOptions = []
        hostingView = hosting
        super.init(frame: NSRect(x: 0, y: 0, width: width, height: 100))
        // Taller than any screen and pinned to the top; the view, as tall as the menu shows,
        // cuts the rest off.
        hosting.frame = NSRect(x: 0, y: 0, width: width, height: Self.hostingHeight)
        clipsToBounds = true
        addSubview(hosting)
        updateHeight = { [weak self] height in self?.contentDidChange(height: height) }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    /// Takes the content's height now, without animation, so the menu opens at it.
    func sizeToFitContent() {
        stopResizing()
        let height = ceil(hostingView.fittingSize.height)
        guard height > 0 else { return }
        contentHeight = height
        setFrameSize(NSSize(width: width, height: height))
    }

    private func contentDidChange(height: CGFloat) {
        let height = ceil(height)
        guard height > 0, height != contentHeight else { return }
        contentHeight = height
        guard window?.isVisible == true else {
            stopResizing()
            return setHeight(height)
        }
        let now = CACurrentMediaTime()
        // A new height mid-resize starts from where the menu is, keeping its speed.
        let speed = resize?.speed(at: now) ?? 0
        let distance = height - frame.height
        resize = Resize(from: frame.height, to: height, start: now,
                        initialVelocity: abs(distance) < 1 ? 0 : speed / distance)
        if resizeLink == nil {
            let link = displayLink(target: self, selector: #selector(stepResize))
            link.add(to: .main, forMode: .common)
            resizeLink = link
        }
    }

    @objc private func stepResize(_ link: CADisplayLink) {
        guard let resize else { return stopResizing() }
        let now = link.targetTimestamp
        let settled = resize.isSettled(at: now)
        setHeight(settled ? resize.to : resize.height(at: now))
        if settled { stopResizing() }
    }

    private func stopResizing() {
        resize = nil
        resizeLink?.invalidate()
        resizeLink = nil
    }

    private func setHeight(_ height: CGFloat) {
        guard height != frame.height else { return }
        let size = NSSize(width: width, height: height)
        setFrameSize(size)
        tellMenu(size)
        // The menu sets its window's new frame without drawing; laid out and drawn now, the new
        // frame and the content in it reach the screen together, rather than the content a frame
        // late, out of place by the step.
        if let window, window.isVisible {
            window.contentView?.layoutSubtreeIfNeeded()
            window.displayIfNeeded()
        }
    }

    /// Tells the menu the item's view changed size.
    private func tellMenu(_ size: NSSize) {
        let implSelector = NSSelectorFromString("_menuImplIfExists")
        let changeSelector = NSSelectorFromString("_menuItem:atIndex:customViewDidChangeSize:")
        guard let item = enclosingMenuItem, let menu = item.menu, menu.responds(to: implSelector),
              let impl = menu.perform(implSelector)?.takeUnretainedValue() as? NSObject,
              let method = class_getInstanceMethod(type(of: impl), changeSelector) else { return }
        let index = menu.index(of: item)
        guard index >= 0 else { return }
        typealias Change = @convention(c) (NSObject, Selector, NSMenuItem, Int, CGSize) -> Void
        unsafeBitCast(method_getImplementation(method), to: Change.self)(impl, changeSelector, item, index, size)
    }

    /// A resize from one height to another, as progress from 0 to 1 along `spring`.
    private struct Resize {
        var from: CGFloat
        var to: CGFloat
        var start: CFTimeInterval
        /// In progress per second.
        var initialVelocity: Double

        func height(at time: CFTimeInterval) -> CGFloat {
            let progress = MenuPanelItemView.spring.value(target: 1.0, initialVelocity: initialVelocity, time: time - start)
            return (from + (to - from) * progress).rounded()
        }

        /// In points per second.
        func speed(at time: CFTimeInterval) -> Double {
            MenuPanelItemView.spring.velocity(target: 1.0, initialVelocity: initialVelocity, time: time - start) * (to - from)
        }

        func isSettled(at time: CFTimeInterval) -> Bool {
            time - start >= MenuPanelItemView.spring.settlingDuration
        }
    }
}
#endif
