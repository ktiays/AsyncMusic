//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import SwiftUI

/// A rounded panel grouping related content.
struct Card<Content: View>: View {
    var title: String?
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let title {
                Text(title)
                    .font(.headline)
            }
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.fill.quinary, in: RoundedRectangle(cornerRadius: 20, style: .continuous))
    }
}

/// A speaker with the channel it plays (L, R, or LR for the full mix).
struct SpeakerBadge: View {
    var channel: SpeakerChannel
    var size: CGFloat = 36
    var isDimmed = false

    var body: some View {
        ZStack {
            Circle()
                .fill(channel == .full ? AnyShapeStyle(.fill.secondary) : AnyShapeStyle(channel.tint.gradient))
            Text(channel.badge)
                .font(.system(size: size * (channel == .full ? 0.32 : 0.42), weight: .bold, design: .rounded))
                .foregroundStyle(channel == .full ? AnyShapeStyle(.secondary) : AnyShapeStyle(.white))
        }
        .frame(width: size, height: size)
        .opacity(isDimmed ? 0.4 : 1)
        .accessibilityLabel(channel.title)
    }
}

/// A small colored dot with a label, for connection and sync states.
struct StatusLabel: View {
    enum Tone {
        case good, pending, bad, neutral

        var color: Color {
            switch self {
            case .good: .green
            case .pending: .orange
            case .bad: .red
            case .neutral: .secondary
            }
        }
    }

    var text: String
    var tone: Tone

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(tone.color)
                .frame(width: 8, height: 8)
            Text(text)
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }
}

/// A large tappable row with an icon, a title and an explanation.
struct ChoiceRow: View {
    var systemImage: String
    var title: String
    var subtitle: String
    var isSelected = false
    var showsChevron = false

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: systemImage)
                .font(.title2)
                .foregroundStyle(isSelected ? AnyShapeStyle(.white) : AnyShapeStyle(.tint))
                .frame(width: 44, height: 44)
                .background(isSelected ? AnyShapeStyle(.tint) : AnyShapeStyle(.tint.quinary), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.headline)
                    .foregroundStyle(.primary)
                Text(subtitle)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.leading)
            }
            Spacer(minLength: 0)
            if showsChevron {
                Image(systemName: "chevron.right")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.tertiary)
            } else if isSelected {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.tint)
            }
        }
        .contentShape(Rectangle())
    }
}

extension View {
    /// Lays content out in a centered, readable column inside a scroll view.
    func readableColumn() -> some View {
        frame(maxWidth: 560)
            .padding(.horizontal, 16)
            .padding(.vertical, 20)
            .frame(maxWidth: .infinity)
    }
}

/// Explains that Local Network access is off, which silently cuts this device off from the
/// group, and links to the setting.
struct LocalNetworkBanner: View {
    @Environment(\.openURL) private var openURL

    var body: some View {
        Card {
            Label {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Local Network Access Is Off")
                        .font(.headline)
                    Text("Other devices can’t hear this one. Allow AsyncMusic under Privacy & Security › Local Network in Settings.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
            } icon: {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
            }
            Button("Open Settings") {
                if let url = Self.settingsURL {
                    openURL(url)
                }
            }
            .buttonStyle(.bordered)
        }
    }

    private static var settingsURL: URL? {
        #if os(macOS)
        URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork")
        #else
        URL(string: UIApplication.openSettingsURLString)
        #endif
    }
}
