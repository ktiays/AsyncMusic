//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

/// What the share alert popover under the menu bar icon shows: one row per new share, newest
/// first. Closing it (click elsewhere, or waiting) is how a share is ignored; it stays listed in
/// the panel.
struct ShareAlertsView: View {
    let center: ShareCenter
    /// Told when the pointer enters or leaves, so the alert stays open while it's being read.
    var onHover: (Bool) -> Void

    private var alerts: [NearbyShare] { center.alerts }
    private static let maxRows = 3

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(alerts.prefix(Self.maxRows).enumerated()), id: \.element.id) { index, share in
                if index > 0 { Divider().padding(.vertical, 10) }
                AlertRow(share: share, iconSize: 36) { center.join(share) }
            }
            if alerts.count > Self.maxRows {
                Divider().padding(.vertical, 10)
                MoreSharesLabel(count: alerts.count - Self.maxRows)
            }
        }
        .padding(14)
        .frame(width: 320)
        .fixedSize(horizontal: false, vertical: true)
        .alertControlsActive()
        .onHover(perform: onHover)
        .animation(.default, value: alerts.map(\.id))
    }
}

/// What the share alert window in the middle of the screen shows, when there's no menu bar icon:
/// one share large, several as a list, with a close button in the corner.
///
/// Everything at the window's edge is concentric with its corners: the close button's circle
/// shares the corner's center, and the Join button's corners are the window's, less the margin.
struct ShareAlertWindowView: View {
    let alerts: [NearbyShare]
    var join: (NearbyShare) -> Void
    var dismiss: () -> Void
    var onHover: (Bool) -> Void

    private static let maxRows = 4
    /// The window's corner radius (`AlertWindow` applies it).
    static let cornerRadius: CGFloat = 26
    /// The close button's circle as drawn; the glass style adds `glassPadding` around its label.
    private static let closeSize: CGFloat = 32
    private static let glassPadding: CGFloat = 4
    /// Puts the close button's center on the corner's.
    private static let closeInset = cornerRadius - closeSize / 2
    /// Between the Join button and the window's edges.
    private static let margin: CGFloat = 14
    private static let joinRadius = cornerRadius - margin
    private static let listMargin: CGFloat = 20

    var body: some View {
        Group {
            if alerts.count > 1 {
                list
            } else if let share = alerts.first {
                single(share)
            }
        }
        .alertControlsActive()
        .onHover(perform: onHover)
        .animation(.default, value: alerts.map(\.id))
    }

    private func single(_ share: NearbyShare) -> some View {
        VStack(spacing: 0) {
            DeviceIcon(share: share, size: 104)
                .padding(.top, 34)
            Text(share.name)
                .font(.title2.weight(.semibold))
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .padding(.top, 14)
            Text("Sharing \(share.summary)")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .padding(.top, 4)
                .padding(.horizontal, 10)
            Button { join(share) } label: {
                Text("Join").font(.body.weight(.semibold)).frame(maxWidth: .infinity)
            }
            .buttonStyle(.glassProminent)
            .buttonBorderShape(.roundedRectangle(radius: Self.joinRadius))
            .controlSize(.extraLarge)
            .padding(.top, 26)
        }
        .padding([.horizontal, .bottom], Self.margin)
        .frame(width: 320)
        .overlay(alignment: .topLeading) {
            closeButton.padding(Self.closeInset)
        }
    }

    private var list: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 10) {
                closeButton
                Text("\(alerts.count) Shares Nearby").font(.headline)
            }
            .padding(.top, Self.closeInset)
            // Out of the rows' margin, into the corner.
            .padding(.leading, Self.closeInset - Self.listMargin)
            .padding(.bottom, 12)
            ForEach(Array(alerts.prefix(Self.maxRows).enumerated()), id: \.element.id) { index, share in
                if index > 0 { Divider().padding(.vertical, 10) }
                AlertRow(share: share, iconSize: 44) { join(share) }
            }
            if alerts.count > Self.maxRows {
                Divider().padding(.vertical, 10)
                MoreSharesLabel(count: alerts.count - Self.maxRows)
            }
        }
        .padding(.horizontal, Self.listMargin)
        .padding(.bottom, Self.listMargin)
        .frame(width: 380)
    }

    private var closeButton: some View {
        Button(action: dismiss) {
            Image(systemName: "xmark")
                .font(.system(size: 12, weight: .bold))
                .frame(width: Self.closeSize - 2 * Self.glassPadding, height: Self.closeSize - 2 * Self.glassPadding)
        }
        .buttonStyle(.glass)
        .buttonBorderShape(.circle)
        .help("Close")
        .accessibilityLabel("Close")
    }
}

private struct AlertRow: View {
    let share: NearbyShare
    let iconSize: CGFloat
    var join: () -> Void

    var body: some View {
        HStack(spacing: 10) {
            DeviceIcon(share: share, size: iconSize)
            VStack(alignment: .leading, spacing: 1) {
                Text(share.name)
                    .font(.headline)
                    .lineLimit(1)
                Text("Sharing \(share.summary)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 8)
            Button("Join", action: join)
                .buttonStyle(.borderedProminent)
        }
    }
}

private struct MoreSharesLabel: View {
    let count: Int

    var body: some View {
        Text("\(count) more in AsyncMusic")
            .font(.subheadline)
            .foregroundStyle(.secondary)
    }
}

private extension View {
    /// The alert never takes focus from the app in use, and controls in a window without it draw
    /// as inactive (a grey Join button).
    func alertControlsActive() -> some View {
        environment(\.controlActiveState, .key)
    }
}
