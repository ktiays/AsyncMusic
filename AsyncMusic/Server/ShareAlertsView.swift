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
struct ShareAlertWindowView: View {
    let center: ShareCenter
    var onHover: (Bool) -> Void

    private var alerts: [NearbyShare] { center.alerts }
    private static let maxRows = 4

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
            DeviceIcon(share: share, size: 96)
                .padding(.top, 16)
            Text(share.name)
                .font(.title3.weight(.semibold))
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .padding(.top, 14)
            Text("Sharing \(share.summary)")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .padding(.top, 3)
            Button { center.join(share) } label: {
                Text("Join").frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .padding(.top, 20)
        }
        .padding(20)
        .frame(width: 300)
        .overlay(alignment: .topTrailing) {
            closeButton.padding(10)
        }
    }

    private var list: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("\(alerts.count) Shares Nearby").font(.headline)
                Spacer()
                closeButton
            }
            .padding(.bottom, 14)
            ForEach(Array(alerts.prefix(Self.maxRows).enumerated()), id: \.element.id) { index, share in
                if index > 0 { Divider().padding(.vertical, 10) }
                AlertRow(share: share, iconSize: 44) { center.join(share) }
            }
            if alerts.count > Self.maxRows {
                Divider().padding(.vertical, 10)
                MoreSharesLabel(count: alerts.count - Self.maxRows)
            }
        }
        .padding(18)
        .frame(width: 360)
    }

    private var closeButton: some View {
        Button { center.dismissAlerts() } label: {
            Image(systemName: "xmark")
                .font(.system(size: 13, weight: .semibold))
                .frame(width: 28, height: 28)
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
