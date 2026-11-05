//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import SwiftUI

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
