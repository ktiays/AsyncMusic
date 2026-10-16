//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Charts
import SwiftUI
import UniformTypeIdentifiers

/// Everything about sync, streaming and output in detail, for diagnosing problems: clock
/// estimates and their history, render statistics, manual latency adjustment, raw playback
/// controls (metronome, channel test) and the log.
struct DiagnosticsView: View {
    let session: ClockSyncSession
    @State private var isImportingFile = false

    var body: some View {
        Form {
            Section {
                LabeledContent("Role", value: session.role.rawValue)
                LabeledContent("Device", value: session.deviceName)
                LabeledContent("Device ID", value: String(format: "%016llx", session.identity.id))
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
                speakersSection
                playbackSection
                outputSection
            }

            logSection
        }
        .formStyle(.grouped)
        .navigationTitle("Diagnostics")
        .fileImporter(isPresented: $isImportingFile, allowedContentTypes: [.audio]) { result in
            if case let .success(url) = result {
                session.loadTrack(from: url)
            }
        }
    }

    // MARK: - Leader

    private var leaderSection: some View {
        Section("Leader") {
            LabeledContent("Followers", value: "\(session.followerCount)")
            ForEach(session.members) { member in
                LabeledContent(member.name, value: String(format: "%016llx%@", member.id, member.isActive ? "" : " · silent"))
            }
            LeaderClockLabel(map: .identity())
        }
    }

    // MARK: - Follower

    @ViewBuilder
    private var followerSections: some View {
        Section("Leader") {
            LabeledContent("Connected to", value: session.connectedLeader ?? "–")
            LabeledContent("Discovered", value: "\(session.discoveredLeaders.count)")
        }

        if let round = session.latestRound {
            Section("Estimate") {
                if let map = round.map {
                    LabeledContent("Offset", value: String(format: "%.3f ms", Double(map.offset(atLocal: round.localTime)) / 1e6))
                    LabeledContent("Skew", value: String(format: "%+.2f ppm", map.skewPPM()))
                    LeaderClockLabel(map: map)
                }
                LabeledContent("Fit jitter (RMS)", value: String(format: "%.0f µs", round.residualRMS / 1e3))
                if let minimum = round.minimumRoundTrip {
                    LabeledContent("Min RTT", value: String(format: "%.2f ms", Double(minimum) / 1e6))
                    LabeledContent("Asymmetry bound", value: String(format: "± %.2f ms", Double(minimum) / 2e6))
                }
                LabeledContent("Rounds", value: "\(round.acceptedCount) accepted · \(round.rejectedCount) rejected · \(round.windowCount) in window")
                LabeledContent("Last round", value: "\(round.probesReceived)/\(round.probesSent) replies · \(round.verdict.name)")
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
                        y: .value("RTT", Double(best.roundTrip()) / 1e6)
                    )
                }
            }
        }
        .frame(height: 140)
    }

    // MARK: - Speakers

    private var speakersSection: some View {
        Section("Speakers") {
            LabeledContent("Layout", value: (session.role == .leader ? session.speakerLayout : session.remoteLayout).title)
            LabeledContent("This device plays", value: session.player.channel.title)
            if session.role == .leader {
                ForEach(session.speakerLayout.channels, id: \.rawValue) { channel in
                    let device = session.channelAssignments[channel]
                    LabeledContent(channel.title, value: device.map { session.name(ofDevice: $0) ?? String(format: "%016llx (not here)", $0) } ?? "–")
                }
                LabeledContent("Split active", value: session.activeChannels.isEmpty ? "No (full mix everywhere)" : "Yes")
            }
        }
    }

    // MARK: - Playback

    private var playbackSection: some View {
        Section("Playback") {
            LabeledContent("Now", value: session.player.playback.isPlaying() ? session.describe(session.player.playback.source) : "Stopped")
            if session.player.isWaitingForAudio {
                Label("Waiting for audio from the leader…", systemImage: "hourglass")
                    .foregroundStyle(.secondary)
            }

            // Followers get files streamed from the leader; only the leader needs one.
            if session.role == .leader {
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

                HStack {
                    Button("Metronome") { session.play(source: PlaybackSource.metronome) }
                    Button("Channel Test") { session.play(source: PlaybackSource.channelTest) }
                    Button("Play File") {
                        if let track = session.selectedTrack {
                            session.play(source: track.id)
                        }
                    }
                    .disabled(session.selectedTrack == nil)
                    Spacer()
                    Button("Stop", role: .destructive) { session.stopPlayback() }
                        .disabled(!session.player.playback.isPlaying())
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
                Button("Copy Report") { copyReport() }
                    .font(.caption)
                Button("Clear") { session.clearLog() }
                    .font(.caption)
            }
        }
    }
}

extension DiagnosticsView {
    /// Puts a plain-text summary of this device's state and its log on the clipboard, to paste
    /// into a bug report.
    private func copyReport() {
        let player = session.player
        let stats = player.renderStats
        var lines = [
            "AsyncMusic diagnostics",
            "Device: \(session.deviceName) (\(String(format: "%016llx", session.identity.id)))",
            "Role: \(session.role.rawValue) · \(session.status)",
            "Engine: \(player.isRunning ? "running" : "stopped")\(player.engineError.map { " · error: \($0)" } ?? "")",
            String(format: "Output: %.0f Hz, latency %.2f ms, extra delay %+.1f ms", player.outputSampleRate, player.reportedLatency * 1e3, player.manualOffset),
            "Playing: \(player.playback.isPlaying() ? session.describe(player.playback.source) : "nothing")\(player.isWaitingForAudio ? " (waiting for audio)" : "") · channel \(player.channel.title)",
            String(format: "Render: playing %@, position %.3f s, error %+.0f µs, buffered %.1f s, gaps %d, repositions %d, peak %.4f, host time %@",
                   stats.isPlaying ? "yes" : "no", stats.mediaSeconds, stats.errorMicroseconds, stats.bufferedSeconds, stats.gaps, stats.jumps, stats.outputPeak, stats.hostTimeValid ? "valid" : "INVALID"),
            "",
            "Log (oldest first):",
        ]
        lines += session.log.map { "\($0.date.formatted(date: .omitted, time: .standard))  \($0.text)" }
        let report = lines.joined(separator: "\n")
        #if os(macOS)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report, forType: .string)
        #else
        UIPasteboard.general.string = report
        #endif
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
            LabeledContent("Stream gaps", value: "\(stats.gaps)")
            LabeledContent("Output level", value: stats.isPlaying ? (stats.outputPeak > 0 ? String(format: "%.1f dBFS", 20 * log10(stats.outputPeak)) : "silent") : "–")
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
    NavigationStack {
        DiagnosticsView(session: ClockSyncSession())
    }
}
