//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

/// Listening: shares around this device on a radar, and the one joined.
struct ListenSection: View {
    let center: ShareCenter

    var body: some View {
        let state = center.state
        VStack(alignment: .leading, spacing: 22) {
            NetworkProblems(state: state)
            if let joined = state.joined {
                JoinedView(center: center, share: joined)
                if !state.nearbyOthers.isEmpty {
                    NearbyList(title: "Also sharing nearby", shares: state.nearbyOthers, highlightsFirst: false) { center.join($0) }
                }
            } else {
                // Newest first, so the newest always has a place on the radar.
                Radar(shares: newestFirst(state.nearbyOthers)) { center.join($0) }
                if state.nearbyOthers.isEmpty {
                    VStack(spacing: 6) {
                        Text("Listening for shares").font(.headline)
                        Text("When a Mac, PC or phone on this Wi-Fi starts sharing, it appears around you. Tap it to play along in sync.")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.horizontal, 12)
                } else {
                    let count = state.nearbyOthers.count
                    NearbyList(title: count == 1 ? "1 sharing nearby" : "\(count) sharing nearby", shares: newestFirst(state.nearbyOthers), highlightsFirst: true) { center.join($0) }
                }
            }
        }
    }

    private func newestFirst(_ shares: [NearbyShare]) -> [NearbyShare] {
        shares.sorted { $0.firstSeen > $1.firstSeen }
    }
}

// MARK: - Radar

/// This device in the middle of rings, with the shares nearby around it; the newest one is
/// ringed in the accent color. Tapping a share joins it.
private struct Radar: View {
    let shares: [NearbyShare]
    var join: (NearbyShare) -> Void

    private static let height: CGFloat = 350
    /// Where the shares sit, as (angle in degrees, distance from the center): the corners of a
    /// pentagon, far enough apart that names don't collide. More shares are only listed below.
    private static let slots: [(Double, CGFloat)] = [(-126, 128), (-54, 128), (18, 126), (162, 126), (90, 132)]

    var body: some View {
        let newest = shares.max { $0.firstSeen < $1.firstSeen }?.id
        GeometryReader { geometry in
            // A little above the middle: names hang below the bubbles.
            let middle = CGPoint(x: geometry.size.width / 2, y: geometry.size.height / 2 - 10)
            ZStack {
                Rings(isSearching: shares.isEmpty)
                    .position(middle)
                ZStack {
                    Circle().fill(Color.accentColor)
                    Image(systemName: "iphone")
                        .font(.system(size: 24, weight: .semibold))
                        .foregroundStyle(.white)
                }
                .frame(width: 56, height: 56)
                .position(middle)
                .accessibilityLabel("This device")

                ForEach(Array(shares.prefix(Self.slots.count).enumerated()), id: \.element.id) { index, share in
                    let (angle, distance) = Self.slots[index]
                    let radians = angle * .pi / 180
                    Bubble(share: share, isNewest: share.id == newest) { join(share) }
                        .position(x: middle.x + cos(radians) * distance, y: middle.y + sin(radians) * distance)
                        .transition(.scale.combined(with: .opacity))
                }
            }
        }
        .frame(height: Self.height)
        .animation(.spring(duration: 0.5, bounce: 0.3), value: shares.map(\.id))
    }
}

private struct Rings: View {
    let isSearching: Bool

    var body: some View {
        ZStack {
            // Guides, barely there: the shares are what matter.
            Circle().stroke(.primary.opacity(0.07), lineWidth: 1).frame(width: 300, height: 300)
            Circle().stroke(.primary.opacity(0.11), lineWidth: 1).frame(width: 200, height: 200)
            Circle()
                .fill(Color.accentColor.opacity(0.10))
                .overlay(Circle().stroke(Color.accentColor.opacity(0.35), lineWidth: 1))
                .frame(width: 104, height: 104)
            if isSearching {
                // A ring sweeping outward while nothing is around yet.
                Circle()
                    .stroke(Color.accentColor.opacity(0.5), lineWidth: 1.5)
                    .frame(width: 104, height: 104)
                    .phaseAnimator([false, true]) { ring, expanded in
                        ring.scaleEffect(expanded ? 2.9 : 1).opacity(expanded ? 0 : 0.9)
                    } animation: { expanded in
                        expanded ? .easeOut(duration: 2.4) : .linear(duration: 0)
                    }
            }
        }
        .accessibilityHidden(true)
    }
}

private struct Bubble: View {
    let share: NearbyShare
    let isNewest: Bool
    var join: () -> Void

    var body: some View {
        Button(action: join) {
            VStack(spacing: 5) {
                Image(systemName: share.deviceKind.symbol)
                    .font(.system(size: isNewest ? 26 : 22, weight: .regular))
                    .frame(width: isNewest ? 64 : 54, height: isNewest ? 64 : 54)
                    .background(.background.secondary, in: .circle)
                    .overlay(Circle().stroke(isNewest ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.quaternary), lineWidth: isNewest ? 1.5 : 1))
                VStack(spacing: 0) {
                    Text(share.name)
                        .font(.footnote.weight(.semibold))
                        .lineLimit(1)
                    if !share.source.isEmpty {
                        Text(share.source)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
                // On the screen's own color, so no ring runs through the name.
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(.background, in: .rect(cornerRadius: 8))
            }
            .frame(maxWidth: 120)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Join \(share.name), \(share.summary)")
    }
}

// MARK: - Nearby list

/// The shares again, as rows in thumb reach.
private struct NearbyList: View {
    let title: String
    let shares: [NearbyShare]
    let highlightsFirst: Bool
    var join: (NearbyShare) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.subheadline.weight(.semibold))
                .padding(.horizontal, 12)
                .padding(.top, 10)
                .padding(.bottom, 4)
            ForEach(Array(shares.enumerated()), id: \.element.id) { index, share in
                let isFirst = highlightsFirst && index == 0
                HStack(spacing: 12) {
                    Image(systemName: share.deviceKind.symbol)
                        .font(.system(size: 18))
                        .foregroundStyle(isFirst ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.secondary))
                        .frame(width: 40, height: 40)
                        .background(isFirst ? AnyShapeStyle(Color.accentColor.opacity(0.14)) : AnyShapeStyle(.fill.tertiary), in: .rect(cornerRadius: 12))
                    VStack(alignment: .leading, spacing: 2) {
                        Text(share.name).font(.body.weight(.semibold)).lineLimit(1)
                        Text(share.summary).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer(minLength: 8)
                    if isFirst {
                        Button("Join") { join(share) }
                            .buttonStyle(.borderedProminent)
                            .buttonBorderShape(.capsule)
                    } else {
                        Button("Join") { join(share) }
                            .buttonStyle(.bordered)
                            .buttonBorderShape(.capsule)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 10)
                .background(isFirst ? AnyShapeStyle(.fill.quaternary) : AnyShapeStyle(.clear), in: .rect(cornerRadius: 20))
            }
        }
        .padding(8)
        .card()
    }
}

// MARK: - Joined

private struct JoinedView: View {
    let center: ShareCenter
    let share: NearbyShare

    var body: some View {
        let state = center.state
        let isPaused = center.isListeningPaused
        VStack(alignment: .leading, spacing: 22) {
            Waveform(isActive: state.isPlaying && !isPaused)
                .frame(height: 170)
                .frame(maxWidth: .infinity)
                .card(cornerRadius: 32)
                .overlay(alignment: .topTrailing) {
                    SyncChip(status: state.syncStatus).padding(14)
                }

            VStack(alignment: .leading, spacing: 4) {
                Text("LISTENING TO")
                    .font(.footnote.weight(.semibold))
                    .tracking(0.4)
                    .foregroundStyle(.tint)
                Text(share.name)
                    .font(.largeTitle.bold())
                    .lineLimit(2)
                Text(share.summary).font(.body).foregroundStyle(.secondary)
            }

            if state.joinedLayout == .stereoPair {
                StereoPairTiles(channel: state.channel) { center.requestChannel($0) }
            } else {
                PanelSection {
                    PanelRow("Plays as") { Text(ServerState.Channel.full.title).foregroundStyle(.secondary) }
                }
            }

            HStack(spacing: 16) {
                Button {
                    center.setListeningPaused(!isPaused)
                } label: {
                    Image(systemName: isPaused ? "play.fill" : "pause.fill")
                        .font(.title2)
                        .contentTransition(.symbolEffect(.replace))
                        .foregroundStyle(Color(.systemBackground))
                        .frame(width: 64, height: 64)
                        .background(.primary, in: .circle)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(isPaused ? "Play on This \(DeviceWord.current)" : "Pause on This \(DeviceWord.current)")
                VStack(alignment: .leading, spacing: 6) {
                    Text(isPaused ? "Paused here · still in sync" : "Volume on this \(DeviceWord.current)")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Slider(value: Binding(get: { Double(state.volume) }, set: { center.setVolume(Float($0)) }), in: 0 ... 1)
                        .accessibilityLabel("Volume")
                }
            }

            Button(role: .destructive) {
                center.leave()
            } label: {
                Text("Leave").font(.body.weight(.semibold)).frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .buttonBorderShape(.capsule)
            .controlSize(.large)
            .tint(.red)
        }
    }
}

/// Bars that move with the music while it plays here, and rest when it doesn't.
private struct Waveform: View {
    let isActive: Bool

    private static let rest: [CGFloat] = [0.2, 0.42, 0.7, 0.5, 0.88, 0.58, 0.34, 0.8, 0.46, 0.66, 0.3, 0.54, 0.26, 0.14]

    var body: some View {
        TimelineView(.animation(paused: !isActive)) { context in
            let time = context.date.timeIntervalSinceReferenceDate
            HStack(alignment: .center, spacing: 8) {
                ForEach(Self.rest.indices, id: \.self) { index in
                    let base = Self.rest[index]
                    let wave = isActive ? 0.5 + 0.5 * sin(time * (2.2 + Double(index % 4) * 0.7) + Double(index) * 0.9) : 0.5
                    Capsule()
                        .fill(isActive ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.tertiary))
                        .frame(width: 8, height: max(10, 96 * (base * 0.55 + CGFloat(wave) * base * 0.45)))
                }
            }
        }
        .animation(.easeOut(duration: 0.3), value: isActive)
        .accessibilityHidden(true)
    }
}

/// Left and Right as two big tiles; this device's side is filled. Tapping the other side takes
/// it, and the device there moves over.
private struct StereoPairTiles: View {
    let channel: ServerState.Channel
    var request: (ServerState.Channel) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text("Stereo pair").font(.subheadline.weight(.semibold))
                Spacer()
                Text(channel == .full ? "Waiting for the other speaker" : "Tap a side to play it")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 10) {
                tile(.left, letter: "L", title: "Left channel")
                tile(.right, letter: "R", title: "Right channel")
            }
            .sensoryFeedback(.selection, trigger: channel)
        }
    }

    private func tile(_ side: ServerState.Channel, letter: String, title: String) -> some View {
        let isMine = channel == side
        return Button {
            if !isMine { request(side) }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Spacer(minLength: 0)
                Text(letter).font(.system(size: 30, weight: .bold, design: .rounded))
                Text(isMine ? "You" : "Other speaker").font(.subheadline.weight(.semibold))
                Text(title).font(.caption).opacity(0.75)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .frame(height: 116)
            .padding(.horizontal, 16)
            .padding(.vertical, 14)
            .foregroundStyle(isMine ? AnyShapeStyle(.white) : AnyShapeStyle(.primary))
            .background(isMine ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.fill.quinary), in: .rect(cornerRadius: 24))
            .overlay(RoundedRectangle(cornerRadius: 24).stroke(isMine ? AnyShapeStyle(.clear) : AnyShapeStyle(.quaternary), lineWidth: 1))
            .contentShape(.rect(cornerRadius: 24))
        }
        .buttonStyle(.plain)
        .animation(.snappy, value: isMine)
        .accessibilityLabel("\(title), \(isMine ? "played by this device" : "played by the other speaker")")
        .accessibilityAddTraits(isMine ? .isSelected : [])
    }
}

/// "In sync" in green, or how the connection is going.
private struct SyncChip: View {
    let status: SyncStatus?

    var body: some View {
        let (text, color): (String, Color) = switch status {
        case .syncing: ("In sync", .green)
        case .localNetworkDenied: ("No Local Network access", .red)
        case .reconnecting: ("Reconnecting…", .orange)
        case .failed: ("Connection failed", .red)
        case .connecting, nil: ("Connecting…", .orange)
        }
        HStack(spacing: 7) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(text).font(.footnote.weight(.semibold))
        }
        .foregroundStyle(color)
        .padding(.horizontal, 12)
        .frame(height: 32)
        .background(color.opacity(0.13), in: .capsule)
    }
}
