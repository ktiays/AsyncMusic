//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct AppIcon: View {
    let url: URL?
    var size: CGFloat = 26

    var body: some View {
        if let url {
            Image(nsImage: NSWorkspace.shared.icon(forFile: url.path)).resizable().frame(width: size, height: size)
        } else {
            Image(systemName: "waveform").frame(width: size, height: size)
        }
    }
}

/// The app's mark on a dark tile.
struct AppGlyph: View {
    var size: CGFloat

    var body: some View {
        ShareIcon(emphasis: .playing)
            .foregroundStyle(.white)
            .padding(size * 0.14)
            .frame(width: size, height: size)
            .background(Color(white: 0.12), in: .rect(cornerRadius: size * 0.25))
            .accessibilityHidden(true)
    }
}

/// The device sharing: the system's picture of its exact model (as AirDrop and Finder show), or a
/// symbol for its kind when the model is unknown here.
struct DeviceIcon: View {
    let share: NearbyShare
    var size: CGFloat

    var body: some View {
        Group {
            if let image = Self.picture(forModel: share.model) {
                Image(nsImage: image)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
            } else {
                Image(systemName: symbol)
                    .resizable()
                    .scaledToFit()
                    .fontWeight(.light)
                    .foregroundStyle(.secondary)
                    .padding(size * 0.12)
            }
        }
        .frame(width: size, height: size)
        .accessibilityHidden(true)
    }

    private var symbol: String {
        switch share.deviceKind {
        case .laptop: "laptopcomputer"
        case .desktop: "desktopcomputer"
        case .phone: "smartphone"
        case .tablet: "ipad.landscape"
        case .unknown: "hifispeaker"
        }
    }

    private static var pictures: [String: NSImage?] = [:]

    /// The system's icon for a model identifier such as "Mac16,10", if it knows the model.
    static func picture(forModel model: String) -> NSImage? {
        guard !model.isEmpty else { return nil }
        if let known = pictures[model] { return known }
        let type = UTType(tag: model, tagClass: UTTagClass(rawValue: "com.apple.device-model-code"), conformingTo: nil)
        let picture = type.flatMap { $0.isDynamic ? nil : NSWorkspace.shared.icon(for: $0) }
        pictures[model] = picture
        return picture
    }
}

/// What kind of source a share plays, on a tinted tile.
struct DeviceTile: View {
    let kind: ShareKind
    var size: CGFloat

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: size * 0.45, weight: .medium))
            .foregroundStyle(.orange)
            .frame(width: size, height: size)
            .background(.orange.opacity(0.15), in: .circle)
            .accessibilityHidden(true)
    }

    private var symbol: String {
        switch kind {
        case .file: "music.note"
        case .system: "desktopcomputer"
        case .application, .unknown: "dot.radiowaves.left.and.right"
        }
    }
}
#endif
