//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

/// The app's mark: a music note between two pairs of arcs, on a 24-unit grid. The arcs and the
/// note's stem share one stroke width, so the whole mark has a single weight.
struct ShareIcon: View {
    enum Emphasis {
        /// Listening: the outer arcs faint.
        case listening
        /// Joined a share: everything full strength.
        case playing
        /// Sharing: full strength and a little heavier.
        case sharing
    }

    var emphasis: Emphasis = .listening
    var lineWidth: CGFloat = 2
    var showsBadge = false

    var body: some View {
        Canvas { context, size in
            let scale = min(size.width, size.height) / 24
            context.translateBy(x: (size.width - 24 * scale) / 2, y: (size.height - 24 * scale) / 2)
            context.scaleBy(x: scale, y: scale)
            let width = emphasis == .sharing ? lineWidth * 1.2 : lineWidth
            let style = StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round)
            let shading = GraphicsContext.Shading.foreground

            var outer = context
            outer.opacity = emphasis == .listening ? 0.45 : 1
            if showsBadge {
                // Leave a gap around the badge.
                outer.clip(to: Path(ellipseIn: CGRect(x: 16.2, y: 1.2, width: 7.6, height: 7.6)), options: .inverse)
            }
            outer.stroke(Self.outerArcs, with: shading, style: style)

            context.stroke(Self.innerArcs, with: shading, style: style)
            context.stroke(Self.stem, with: shading, style: style)
            context.fill(Self.head, with: shading)

            if showsBadge {
                context.fill(Path(ellipseIn: CGRect(x: 17.6, y: 2.6, width: 4.8, height: 4.8)), with: shading)
            }
        }
    }

    /// An arc from `start` to `end` around `center`, bulging toward `side` (-1 left, 1 right).
    private static func arc(_ path: inout Path, start: CGPoint, end: CGPoint, center: CGPoint, side: CGFloat) {
        let radius = hypot(start.x - center.x, start.y - center.y)
        let from = Angle(radians: atan2(start.y - center.y, start.x - center.x))
        let to = Angle(radians: atan2(end.y - center.y, end.x - center.x))
        path.move(to: start)
        // Left arcs pass through 180°, right ones through 0°.
        path.addArc(center: center, radius: radius, startAngle: from, endAngle: to, clockwise: side < 0)
    }

    static let innerArcs: Path = {
        var path = Path()
        // Chords 7 units tall, radius 5.5: the centres sit 4.243 units inside.
        arc(&path, start: CGPoint(x: 7, y: 8.5), end: CGPoint(x: 7, y: 15.5), center: CGPoint(x: 11.243, y: 12), side: -1)
        arc(&path, start: CGPoint(x: 17, y: 8.5), end: CGPoint(x: 17, y: 15.5), center: CGPoint(x: 12.757, y: 12), side: 1)
        return path
    }()

    static let outerArcs: Path = {
        var path = Path()
        // Chords 11 units tall, radius 8.5.
        arc(&path, start: CGPoint(x: 3.5, y: 6.5), end: CGPoint(x: 3.5, y: 17.5), center: CGPoint(x: 9.981, y: 12), side: -1)
        arc(&path, start: CGPoint(x: 20.5, y: 6.5), end: CGPoint(x: 20.5, y: 17.5), center: CGPoint(x: 14.019, y: 12), side: 1)
        return path
    }()

    static let stem: Path = {
        var path = Path()
        path.move(to: CGPoint(x: 11.95, y: 14.4))
        path.addLine(to: CGPoint(x: 11.95, y: 9.2))
        path.addQuadCurve(to: CGPoint(x: 14.05, y: 11.3), control: CGPoint(x: 12.35, y: 10.6))
        return path
    }()

    static let head: Path = {
        let ellipse = Path(ellipseIn: CGRect(x: -1.9, y: -1.45, width: 3.8, height: 2.9))
        return ellipse.applying(CGAffineTransform(rotationAngle: -20 * .pi / 180).concatenating(CGAffineTransform(translationX: 10.9, y: 14.6)))
    }()
}

enum MenuBarIcon {
    enum State: Equatable {
        case listening
        case playing
        case sharing
    }

    /// A template image for the status item: the system tints it for the menu bar's appearance.
    @MainActor
    static func image(for state: State, badge: Bool) -> NSImage {
        let emphasis: ShareIcon.Emphasis = switch state {
        case .listening: .listening
        case .playing: .playing
        case .sharing: .sharing
        }
        let renderer = ImageRenderer(content: ShareIcon(emphasis: emphasis, showsBadge: badge).foregroundStyle(.black).frame(width: 22, height: 22))
        renderer.scale = 3
        let image = renderer.nsImage ?? NSImage()
        image.isTemplate = true
        image.accessibilityDescription = switch state {
        case .listening: badge ? "AsyncMusic, a share is waiting" : "AsyncMusic, listening"
        case .playing: "AsyncMusic, playing a share"
        case .sharing: "AsyncMusic, sharing"
        }
        return image
    }
}
#endif
