//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI
#if os(macOS)
import AppKit
#endif

/// The app's mark: a music note between two pairs of arcs, on a grid 24 units tall and
/// `gridWidth` wide, centred on x = 12. The arcs and the note's stem share one stroke width, so the
/// whole mark has a single weight.
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

    /// The arcs reach past the 24-unit square so they can sit well clear of the note.
    static let gridWidth: CGFloat = 26.6

    var body: some View {
        Canvas { context, size in
            let scale = min(size.width / Self.gridWidth, size.height / 24)
            context.translateBy(x: (size.width - 24 * scale) / 2, y: (size.height - 24 * scale) / 2)
            context.scaleBy(x: scale, y: scale)
            let width = emphasis == .sharing ? lineWidth * 1.2 : lineWidth
            let style = StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round)
            let shading = GraphicsContext.Shading.foreground

            var outer = context
            outer.opacity = emphasis == .listening ? 0.45 : 1
            if showsBadge {
                // Leave a gap around the badge.
                outer.clip(to: Path(ellipseIn: CGRect(x: 17.7, y: 1.2, width: 7.6, height: 7.6)), options: .inverse)
            }
            outer.stroke(Self.outerArcs, with: shading, style: style)

            context.stroke(Self.innerArcs, with: shading, style: style)
            context.stroke(Self.stem, with: shading, style: style)
            context.fill(Self.stemBase(lineWidth: width), with: shading)
            context.fill(Self.head(lineWidth: width), with: shading)

            if showsBadge {
                context.fill(Path(ellipseIn: CGRect(x: 19.1, y: 2.6, width: 4.8, height: 4.8)), with: shading)
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
        arc(&path, start: CGPoint(x: 6, y: 8.5), end: CGPoint(x: 6, y: 15.5), center: CGPoint(x: 10.243, y: 12), side: -1)
        arc(&path, start: CGPoint(x: 18, y: 8.5), end: CGPoint(x: 18, y: 15.5), center: CGPoint(x: 13.757, y: 12), side: 1)
        return path
    }()

    static let outerArcs: Path = {
        var path = Path()
        // Chords 11 units tall, radius 8.5: the centres sit 6.481 units inside. The gap to the
        // inner arcs is wider than the inner arcs' gap to the note, so the spacing opens outward.
        arc(&path, start: CGPoint(x: 2, y: 6.5), end: CGPoint(x: 2, y: 17.5), center: CGPoint(x: 8.481, y: 12), side: -1)
        arc(&path, start: CGPoint(x: 22, y: 6.5), end: CGPoint(x: 22, y: 17.5), center: CGPoint(x: 15.519, y: 12), side: 1)
        return path
    }()

    static let stem: Path = {
        var path = Path()
        // Round-capped down to just inside the head; `stemBase` carries it the rest of the way.
        path.move(to: CGPoint(x: 11.95, y: 13.4))
        path.addLine(to: CGPoint(x: 11.95, y: 9.2))
        path.addQuadCurve(to: CGPoint(x: 14.05, y: 11.3), control: CGPoint(x: 12.35, y: 10.6))
        return path
    }()

    /// The head's rightmost point, relative to its centre: 1.853 right and 0.262 up. Its edge is
    /// vertical there, so the stem's right edge runs into the head without a notch.
    private static let headRightmost = CGPoint(x: 1.853, y: -0.262)
    private static let headCenterY: CGFloat = 14.6

    /// The stem's square-ended bottom, down to the head's rightmost point.
    static func stemBase(lineWidth: CGFloat) -> Path {
        Path(CGRect(x: 11.95 - lineWidth / 2, y: 13.4, width: lineWidth, height: headCenterY + headRightmost.y - 13.4))
    }

    /// The head, placed so its rightmost point lies on the stem's right edge.
    static func head(lineWidth: CGFloat) -> Path {
        let centerX = 11.95 + lineWidth / 2 - headRightmost.x
        let ellipse = Path(ellipseIn: CGRect(x: -1.9, y: -1.45, width: 3.8, height: 2.9))
        return ellipse.applying(CGAffineTransform(rotationAngle: -20 * .pi / 180).concatenating(CGAffineTransform(translationX: centerX, y: headCenterY)))
    }
}

#if os(macOS)
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
        let renderer = ImageRenderer(content: ShareIcon(emphasis: emphasis, showsBadge: badge).foregroundStyle(.black).frame(width: 22 * ShareIcon.gridWidth / 24, height: 22))
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
