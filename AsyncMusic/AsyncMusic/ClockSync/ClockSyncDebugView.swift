//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Charts
import SwiftUI
import UniformTypeIdentifiers

struct ClockSyncDebugView: View {
    @State private var session = ClockSyncSession()
    @State private var isImportingFile = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Role", selection: Binding(get: { session.role }, set: { session.setRole($0) })) {
                        ForEach(ClockSyncSession.Role.allCases) { role in
                            Text(role.rawValue).tag(role)
                        }
                    }
                    .pickerStyle(.segmented)
                    LabeledContent("Device", value: session.deviceName)
                    LabeledContent("Status", value: session.status)
                }

                switch session.role {
                case .off:
                    EmptyView()
                case .leader:
                    leaderSection
                case .follower:
                    followerSections
                }

                if session.role != .off {
                    playbackSection
                    outputSection
                }

                logSection
            }
            .formStyle(.grouped)
            .navigationTitle("Clock Sync")
            .fileImporter(isPresented: $isImportingFile, allowedContentTypes: [.audio]) { result in
                if case let .success(url) = result {
                    session.loadTrack(from: url)
                }
            }
        }
    }

    // MARK: - Leader

    private var leaderSection: some View {
        Section("Leader") {
            LabeledContent("Followers", value: "\(session.followerCount)")
            LeaderClockLabel(map: .identity)
        }
    }

    // MARK: - Follower

    @ViewBuilder
    private var followerSections: some View {
        Section("Leaders") {
            if session.discoveredLeaders.isEmpty {
                Text("None found on this network yet")
                    .foregroundStyle(.secondary)
            }
            ForEach(session.discoveredLeaders) { leader in
                Button {
                    session.follow(leader)
                } label: {
                    HStack {
                        Text(leader.name)
                        Spacer()
                        if session.connectedLeader == leader.name {
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        }

        if let round = session.latestRound {
            Section("Estimate") {
                if let map = round.map {
                    LabeledContent("Offset", value: String(format: "%.3f ms", Double(map.offset(atLocal: round.localTime)) / 1e6))
                    LabeledContent("Skew", value: String(format: "%+.2f ppm", map.skewPPM))
                    LeaderClockLabel(map: map)
                }
                LabeledContent("Fit jitter (RMS)", value: String(format: "%.0f µs", round.residualRMS / 1e3))
                if let minimum = round.minimumRoundTrip {
                    LabeledContent("Min RTT", value: String(format: "%.2f ms", Double(minimum) / 1e6))
                    LabeledContent("Asymmetry bound", value: String(format: "± %.2f ms", Double(minimum) / 2e6))
                }
                LabeledContent("Rounds", value: "\(round.acceptedCount) accepted · \(round.rejectedCount) rejected · \(round.windowCount) in window")
                LabeledContent("Last round", value: "\(round.probesReceived)/\(round.probesSent) replies · \(String(describing: round.verdict))")
            }

            Section("Residual vs. fitted line (µs)") {
                residualChart
            }

            Section("Best RTT per round (ms)") {
                roundTripChart
            }
        }
    }

    private var residualChart: some View {
        Chart {
            ForEach(session.rounds, id: \.index) { round in
                if let residual = round.residual {
                    PointMark(
                        x: .value("Round", round.index),
                        y: .value("Residual", Double(residual) / 1e3)
                    )
                    .symbolSize(12)
                    .foregroundStyle(round.verdict == .accepted ? Color.accentColor : Color.orange)
                }
            }
            RuleMark(y: .value("Zero", 0))
                .foregroundStyle(.secondary)
        }
        .frame(height: 180)
    }

    private var roundTripChart: some View {
        Chart {
            ForEach(session.rounds, id: \.index) { round in
                if let best = round.best {
                    LineMark(
                        x: .value("Round", round.index),
                        y: .value("RTT", Double(best.roundTrip) / 1e6)
                    )
                }
            }
        }
        .frame(height: 140)
    }

    // MARK: - Playback

    private var playbackSection: some View {
        Section("Playback") {
            LabeledContent("Now", value: session.player.playback.isPlaying ? session.describe(session.player.playback.source) : "Stopped")
            if session.player.isMissingSource {
                Label("This file isn’t loaded on this device. Load the same file to play along.", systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }

            HStack {
                Text(session.selectedTrack?.name ?? "No file loaded")
                    .foregroundStyle(session.selectedTrack == nil ? .secondary : .primary)
                Spacer()
                if session.isLoadingTrack {
                    ProgressView()
                        .controlSize(.small)
                }
                Button("Load File…") { isImportingFile = true }
                    .disabled(session.isLoadingTrack)
            }

            if session.role == .leader {
                HStack {
                    Button("Metronome") { session.play(source: PlaybackSource.metronome) }
                    Button("Play File") {
                        if let track = session.selectedTrack {
                            session.play(source: track.id)
                        }
                    }
                    .disabled(session.selectedTrack == nil)
                    Spacer()
                    Button("Stop", role: .destructive) { session.stopPlayback() }
                        .disabled(!session.player.playback.isPlaying)
                }
                .buttonStyle(.bordered)
            }
        }
    }

    private var outputSection: some View {
        Section("Output") {
            if let error = session.player.engineError {
                Label(error, systemImage: "xmark.octagon")
                    .foregroundStyle(.red)
            }
            LabeledContent("Sample rate", value: String(format: "%.0f Hz", session.player.outputSampleRate))
            LabeledContent("Reported latency", value: String(format: "%.2f ms", session.player.reportedLatency * 1e3))
            if session.player.ioBufferDuration > 0 {
                LabeledContent("IO buffer", value: String(format: "%.2f ms", session.player.ioBufferDuration * 1e3))
            }
            VStack(alignment: .leading) {
                LabeledContent("Extra delay", value: String(format: "%+.1f ms", session.player.manualOffset))
                Slider(value: Binding(get: { session.player.manualOffset }, set: { session.player.manualOffset = $0 }), in: -100 ... 300, step: 0.5)
                Text("Increase if this device sounds late compared with the others.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            RenderStatsView(player: session.player)
        }
    }

    // MARK: - Log

    private var logSection: some View {
        Section {
            ForEach(session.log.suffix(60).reversed()) { line in
                Text(line.text)
                    .font(.caption.monospaced())
                    .textSelection(.enabled)
            }
        } header: {
            HStack {
                Text("Log")
                Spacer()
                Button("Clear") { session.clearLog() }
                    .font(.caption)
            }
        }
    }
}

/// Polls the render thread's statistics.
private struct RenderStatsView: View {
    let player: SyncedPlayer

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.1)) { _ in
            let stats = player.renderStats
            if !stats.hostTimeValid {
                Label("Render timestamps carry no host time", systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.orange)
            }
            LabeledContent("Position", value: stats.isPlaying ? String(format: "%.3f s", stats.mediaSeconds) : "–")
            LabeledContent("Schedule error", value: stats.isPlaying ? String(format: "%+.0f µs", stats.errorMicroseconds) : "–")
            LabeledContent("Rate correction", value: String(format: "%+.1f ppm", stats.correctionPPM))
            LabeledContent("Repositions", value: "\(stats.jumps)")
            if stats.isPlaying, stats.bufferedSeconds > 0 {
                LabeledContent("Buffered ahead", value: String(format: "%.1f s", stats.bufferedSeconds))
            }
        }
    }
}

/// Shows the estimated leader host clock, live. Run it on two devices side by side to eyeball
/// that they agree; the precise numbers are in the estimate above.
private struct LeaderClockLabel: View {
    let map: ClockMap

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1.0 / 30)) { _ in
            let leaderTime = map.leaderTime(fromLocal: HostClock.now())
            let seconds = leaderTime / 1_000_000_000 % 1000
            let milliseconds = leaderTime / 1_000_000 % 1000
            LabeledContent("Leader clock") {
                Text(String(format: "%03lld.%03lld s", seconds, milliseconds))
                    .monospacedDigit()
            }
        }
    }
}

#Preview {
    ClockSyncDebugView()
}
