//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import SwiftUI
import UniformTypeIdentifiers

/// The leader's screen: what plays, how it is spread over the group, and who is in it.
struct HostView: View {
    let session: ClockSyncSession
    @State private var isImportingFile = false

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                if session.isLocalNetworkDenied {
                    LocalNetworkBanner()
                }
                NowPlayingCard(session: session) { isImportingFile = true }
                layoutCard
                if session.speakerLayout == .stereoPair {
                    StereoPairCard(session: session)
                }
                DevicesCard(session: session)
                Button("Stop Hosting", role: .destructive) {
                    session.setRole(.off)
                }
                .buttonStyle(.bordered)
                .padding(.top, 8)
            }
            .readableColumn()
        }
        .navigationTitle("Hosting")
        .fileImporter(isPresented: $isImportingFile, allowedContentTypes: [.audio]) { result in
            if case let .success(url) = result {
                session.loadTrack(from: url)
            }
        }
    }

    private var layoutCard: some View {
        Card(title: "Sound") {
            ForEach(SpeakerLayout.allCases) { layout in
                Button {
                    withAnimation { session.setSpeakerLayout(layout) }
                } label: {
                    ChoiceRow(systemImage: layout.systemImage, title: layout.title, subtitle: layout.summary, isSelected: session.speakerLayout == layout)
                }
                .buttonStyle(.plain)
            }
        }
    }
}

// MARK: - Now playing

private struct NowPlayingCard: View {
    let session: ClockSyncSession
    var chooseMusic: () -> Void

    private var playback: PlaybackState { session.player.playback }
    private var isPlaying: Bool { playback.isPlaying() }

    var body: some View {
        Card {
            HStack(spacing: 14) {
                artwork
                VStack(alignment: .leading, spacing: 4) {
                    Text(title)
                        .font(.title3.weight(.semibold))
                        .lineLimit(2)
                    Text(subtitle)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }

            if isPlaying, let file = session.player.files[playback.source] {
                PlaybackProgress(player: session.player, duration: file.duration)
            }

            HStack {
                Button(action: chooseMusic) {
                    Label("Choose Music", systemImage: "music.note.list")
                }
                .buttonStyle(.bordered)
                .disabled(session.isLoadingTrack)

                Spacer()

                Button {
                    if isPlaying {
                        session.stopPlayback()
                    } else if let track = session.selectedTrack {
                        session.play(source: track.id)
                    }
                } label: {
                    Image(systemName: isPlaying ? "stop.fill" : "play.fill")
                        .font(.title2)
                        .frame(width: 44, height: 44)
                        .contentTransition(.symbolEffect(.replace))
                }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.circle)
                .disabled(!isPlaying && session.selectedTrack == nil)
                .keyboardShortcut(.space, modifiers: [])
                .accessibilityLabel(isPlaying ? "Stop" : "Play")
            }
        }
    }

    private var artwork: some View {
        RoundedRectangle(cornerRadius: 14, style: .continuous)
            .fill(Color.accentColor.gradient)
            .frame(width: 64, height: 64)
            .overlay {
                if session.isLoadingTrack {
                    ProgressView()
                        .tint(.white)
                } else {
                    Image(systemName: isPlaying ? "waveform" : "music.note")
                        .font(.title)
                        .foregroundStyle(.white)
                        .symbolEffect(.variableColor.iterative, isActive: isPlaying)
                }
            }
    }

    private var title: String {
        if isPlaying {
            switch playback.source {
            case PlaybackSource.metronome: return "Metronome"
            case PlaybackSource.channelTest: return "Left & Right Test"
            default: return session.player.files[playback.source]?.name ?? "Music"
            }
        }
        return session.selectedTrack?.name ?? "No Music Chosen"
    }

    private var subtitle: String {
        if session.isLoadingTrack {
            return "Loading…"
        }
        if session.isPreparingPlayback {
            return "Getting speakers ready…"
        }
        if isPlaying {
            let count = 1 + session.members.filter(\.isActive).count
            return count == 1 ? "Playing on this device" : "Playing on \(count) devices"
        }
        return session.selectedTrack == nil ? "Choose a song to start." : "Ready to play"
    }
}

private struct PlaybackProgress: View {
    let player: SyncedPlayer
    let duration: Double

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.25)) { _ in
            let stats = player.renderStats
            let elapsed = stats.isPlaying ? min(max(stats.mediaSeconds, 0), duration) : 0
            VStack(spacing: 4) {
                ProgressView(value: elapsed, total: max(duration, 1))
                HStack {
                    Text(Self.format(elapsed))
                    Spacer()
                    Text("-" + Self.format(duration - elapsed))
                }
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
            }
        }
    }

    private static func format(_ seconds: Double) -> String {
        let total = Int(seconds.rounded(.down))
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

// MARK: - Stereo pair

private struct StereoPairCard: View {
    let session: ClockSyncSession

    private var isTesting: Bool { session.player.playback.source == PlaybackSource.channelTest }

    var body: some View {
        Card(title: "Stereo Pair") {
            HStack(alignment: .top, spacing: 8) {
                slot(.left)
                Button {
                    withAnimation { session.swapChannels() }
                } label: {
                    Image(systemName: "arrow.left.arrow.right")
                }
                .buttonStyle(.bordered)
                .buttonBorderShape(.circle)
                .padding(.top, 18)
                .accessibilityLabel("Swap Left and Right")
                slot(.right)
            }
            .frame(maxWidth: .infinity)

            status

            Button {
                if isTesting {
                    session.stopPlayback()
                } else {
                    session.play(source: PlaybackSource.channelTest)
                }
            } label: {
                Label(isTesting ? "Stop Test" : "Test Left & Right", systemImage: isTesting ? "stop.fill" : "ear")
            }
            .buttonStyle(.bordered)
        }
    }

    private func slot(_ channel: SpeakerChannel) -> some View {
        let device = session.channelAssignments[channel]
        let isPresent = device.map(session.isDevicePresent) ?? false
        return VStack(spacing: 8) {
            SpeakerBadge(channel: channel, size: 60, isDimmed: !isPresent)
            Menu {
                ForEach(candidates, id: \.id) { candidate in
                    Button {
                        withAnimation { session.assign(candidate.id, to: channel) }
                    } label: {
                        if candidate.id == device {
                            Label(candidate.name, systemImage: "checkmark")
                        } else {
                            Text(candidate.name)
                        }
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(device.flatMap(session.name(ofDevice:)) ?? "Choose Device")
                        .lineLimit(1)
                    Image(systemName: "chevron.up.chevron.down")
                        .font(.caption2)
                }
                .font(.subheadline.weight(.medium))
            }
            .menuStyle(.button)
            .buttonStyle(.borderless)
            .fixedSize(horizontal: false, vertical: true)
            Text(slotCaption(device: device, isPresent: isPresent))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
    }

    private func slotCaption(device: UInt64?, isPresent: Bool) -> String {
        guard let device else { return "\u{00A0}" }
        if device == session.identity.id { return "This device" }
        return isPresent ? "Connected" : "Not connected"
    }

    private var candidates: [(id: UInt64, name: String)] {
        [(session.identity.id, session.deviceName)] + session.members.map { ($0.id, $0.name) }
    }

    @ViewBuilder
    private var status: some View {
        if session.isLayoutComplete {
            Label("Stereo is on. For the best effect, sit midway between the two devices.", systemImage: "checkmark.circle.fill")
                .font(.subheadline)
                .foregroundStyle(.green)
        } else {
            Label(waitingText, systemImage: "info.circle")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }

    private var waitingText: String {
        if session.members.isEmpty {
            return "Open AsyncMusic on a second device on this Wi-Fi and tap Join as Speaker. Until then, this device plays the full mix."
        }
        let assigned = SpeakerLayout.stereoPair.channels.compactMap { session.channelAssignments[$0] }
        if let device = assigned.first(where: { !session.isDevicePresent($0) }) {
            return "Waiting for \(session.name(ofDevice: device) ?? "the other device"). Meanwhile every device plays the full mix."
        }
        return "Choose a device for each side. Meanwhile every device plays the full mix."
    }
}

// MARK: - Devices

private struct DevicesCard: View {
    let session: ClockSyncSession

    var body: some View {
        Card(title: "Devices") {
            row(name: session.deviceName, detail: "This device · Host", systemImage: thisDeviceImage, isActive: true, channel: session.player.channel)
            ForEach(session.members) { member in
                Divider()
                row(
                    name: member.name,
                    detail: member.isActive ? "Connected" : "Not responding",
                    systemImage: "hifispeaker.fill",
                    isActive: member.isActive,
                    channel: session.activeChannels[member.id] ?? .full
                )
            }
            if session.members.isEmpty {
                Text("No other devices yet. Open AsyncMusic on another device on this Wi-Fi and choose Join as Speaker.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func row(name: String, detail: String, systemImage: String, isActive: Bool, channel: SpeakerChannel) -> some View {
        HStack(spacing: 12) {
            Image(systemName: systemImage)
                .font(.title3)
                .foregroundStyle(.secondary)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(name)
                    .lineLimit(1)
                StatusLabel(text: detail, tone: isActive ? .good : .pending)
            }
            Spacer()
            SpeakerBadge(channel: channel, size: 30, isDimmed: !isActive)
        }
    }

    private var thisDeviceImage: String {
        #if os(macOS)
        "laptopcomputer"
        #elseif os(visionOS)
        "visionpro"
        #else
        "iphone"
        #endif
    }
}

#Preview {
    NavigationStack {
        HostView(session: ClockSyncSession())
    }
}
