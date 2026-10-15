//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import Network
import Observation
import os
#if canImport(UIKit)
import UIKit
#endif

/// Drives clock sync and synced playback for the UI: either leads (advertises its clock and
/// decides what plays) or follows a discovered leader, and keeps a short history of rounds for
/// the debug view.
@Observable
final class ClockSyncSession {
    enum Role: String, CaseIterable, Identifiable {
        case off = "Off"
        case leader = "Leader"
        case follower = "Follower"

        var id: Self { self }
    }

    struct DiscoveredLeader: Identifiable, Hashable {
        var name: String
        var endpoint: NWEndpoint

        var id: String { name }
    }

    struct LogLine: Identifiable {
        let id = UUID()
        var date: Date
        var text: String
    }

    let deviceName = ClockSyncSession.makeDeviceName()
    let player = SyncedPlayer()

    /// How far ahead the leader schedules a start. Must comfortably exceed the time for the
    /// command to reach followers plus the largest output latency in the group (Bluetooth can
    /// be 200+ ms). Followers that hear about a start late just join mid-stream.
    let startLeadTime: Int64 = 1_000_000_000
    /// Files additionally need their first seconds streamed to every follower before they
    /// start, so they get a longer lead, much like AirPlay's startup delay.
    let fileStartLeadTime: Int64 = 2_000_000_000

    private(set) var isLoadingTrack = false
    /// The file the leader plays with "Play file".
    private(set) var selectedTrack: MediaFile?

    private(set) var role: Role = .off
    private(set) var status = "Idle"

    // Leader
    private(set) var followerCount = 0

    // Follower
    private(set) var discoveredLeaders: [DiscoveredLeader] = []
    private(set) var connectedLeader: String?
    private(set) var latestRound: ClockSyncRound?
    private(set) var rounds: [ClockSyncRound] = []

    private(set) var log: [LogLine] = []

    /// The current mapping to the leader's clock; identity when leading.
    var clockMap: ClockMap? {
        switch role {
        case .leader: .identity
        case .follower: latestRound?.map
        case .off: nil
        }
    }

    private let historyLimit = 300
    private let logLimit = 200

    private var leader: ClockSyncLeader?
    private var follower: ClockSyncFollower?
    private var audioServer: AudioStreamServer?
    private var audioClient: AudioStreamClient?
    private var browser: NWBrowser?

    func setRole(_ newRole: Role) {
        guard newRole != role else { return }
        tearDown()
        role = newRole
        if newRole == .off {
            // Let the app be suspended in the background when it isn't part of a group.
            player.stop()
        } else {
            player.start()
        }
        switch newRole {
        case .off:
            status = "Idle"
        case .leader:
            player.update(clockMap: .identity)
            startLeading()
        case .follower:
            startBrowsing()
        }
    }

    func follow(_ discovered: DiscoveredLeader) {
        follower?.stop()
        rounds = []
        latestRound = nil
        player.update(clockMap: nil)
        player.update(playback: .stopped)
        connectedLeader = discovered.name
        append("Following \(discovered.name)")

        let follower = ClockSyncFollower(endpoint: discovered.endpoint)
        follower.onStateChange = { [weak self] state in
            DispatchQueue.main.async { self?.followerStateChanged(state) }
        }
        follower.onRound = { [weak self] round in
            DispatchQueue.main.async { self?.record(round) }
        }
        follower.onPlayback = { [weak self] state in
            DispatchQueue.main.async { self?.applyRemotePlayback(state) }
        }
        follower.onAudioEndpoint = { [weak self] endpoint in
            DispatchQueue.main.async { self?.connectAudioStream(to: endpoint) }
        }
        self.follower = follower
        follower.start()

        audioClient?.stop()
        audioClient = nil
    }

    /// (Re)connects the audio stream once the clock connection learned where it is.
    private func connectAudioStream(to endpoint: NWEndpoint) {
        guard role == .follower else { return }
        audioClient?.stop()
        append("Audio stream at \(endpoint)")
        let client = AudioStreamClient(endpoint: endpoint)
        client.onStateChange = { [weak self] state in
            DispatchQueue.main.async { self?.audioClientStateChanged(state) }
        }
        client.onStream = { [weak self] source, buffer in
            DispatchQueue.main.async { self?.player.setRemoteStream(buffer, source: source) }
        }
        client.onLog = { [weak self] text in
            DispatchQueue.main.async { self?.append(text) }
        }
        audioClient = client
        client.start()
    }

    func clearLog() {
        log = []
    }

    // MARK: - Playback

    /// Leader only: starts `source` from the top on every device, after `startLeadTime` (or
    /// `fileStartLeadTime` for files, which have to be streamed first).
    func play(source: UInt64) {
        guard role == .leader, let leader else { return }
        let leadTime = source == PlaybackSource.metronome ? startLeadTime : fileStartLeadTime
        let state = PlaybackState(source: source, startLeaderTime: HostClock.now() + leadTime, startMediaTime: 0)
        // Update the local player first: that starts decoding, which is what gets streamed.
        player.update(playback: state)
        leader.setPlayback(state)
        append("Play \(describe(source)) in \(Double(leadTime) / 1e9)s")
    }

    /// Leader only: stops playback on every device.
    func stopPlayback() {
        guard role == .leader, let leader else { return }
        leader.setPlayback(.stopped)
        player.update(playback: .stopped)
        append("Stop")
    }

    /// Reads a file's format and makes it available for playback. Followers must load the same file
    /// to play along; it is matched by content, not name.
    func loadTrack(from url: URL) {
        isLoadingTrack = true
        Task {
            defer { isLoadingTrack = false }
            do {
                let track = try await Task.detached(priority: .userInitiated) {
                    try MediaFile.load(contentsOf: url)
                }.value
                player.add(track)
                selectedTrack = track
                append(String(format: "Loaded “%@” (%.1fs, id %016llx)", track.name, track.duration, track.id))
            } catch {
                append("Load failed: \(error.localizedDescription)")
            }
        }
    }

    func describe(_ source: UInt64) -> String {
        switch source {
        case PlaybackSource.none: "nothing"
        case PlaybackSource.metronome: "metronome"
        default: player.files[source].map { "“\($0.name)”" } ?? String(format: "streamed file %016llx", source)
        }
    }

    private func applyRemotePlayback(_ state: PlaybackState) {
        guard role == .follower else { return }
        player.update(playback: state)
        if state.isPlaying {
            let inSeconds = Double(state.startLeaderTime - (clockMap?.leaderTime(fromLocal: HostClock.now()) ?? state.startLeaderTime)) / 1e9
            append(String(format: "Leader plays %@ (start in %.3fs)", describe(state.source), inSeconds))
        } else {
            append("Leader stopped")
        }
    }

    // MARK: - Leader

    private func startLeading() {
        let leader = ClockSyncLeader(name: deviceName)
        leader.onStateChange = { [weak self] state in
            DispatchQueue.main.async { self?.leaderStateChanged(state) }
        }
        leader.onPlaybackReset = { [weak self] in
            DispatchQueue.main.async {
                guard let self else { return }
                self.player.update(playback: .stopped)
                self.append("This device slept; playback stopped and followers will resync")
            }
        }
        leader.onFollowersChange = { [weak self] count in
            DispatchQueue.main.async {
                guard let self, count != self.followerCount else { return }
                self.followerCount = count
                self.append("Followers: \(count)")
            }
        }
        self.leader = leader
        leader.start()

        let server = AudioStreamServer()
        server.onPortChange = { [weak leader] port in
            leader?.setAudioPort(port)
        }
        server.onReceiversChange = { [weak self] count in
            DispatchQueue.main.async { self?.append("Audio stream receivers: \(count)") }
        }
        server.onError = { [weak self] message in
            DispatchQueue.main.async { self?.append("Audio stream error: \(message)") }
        }
        player.onOutgoingStream = { [weak server] stream in
            server?.setStream(stream)
        }
        audioServer = server
        server.start()
    }

    private func leaderStateChanged(_ state: ClockSyncLeader.State) {
        switch state {
        case .starting:
            status = "Starting listener…"
        case let .advertising(port):
            status = "Advertising as “\(deviceName)”"
            append("Advertising on UDP port \(port)")
        case let .failed(message):
            status = "Failed: \(message)"
            append("Leader error: \(message)")
        case .stopped:
            break
        }
    }

    // MARK: - Follower

    private func startBrowsing() {
        status = "Looking for leaders…"
        let browser = NWBrowser(for: .bonjour(type: ClockSyncNetwork.serviceType, domain: nil), using: ClockSyncNetwork.parameters())
        browser.browseResultsChangedHandler = { [weak self] results, _ in
            let leaders = results.compactMap { result -> DiscoveredLeader? in
                guard case let .service(name, _, _, _) = result.endpoint else { return nil }
                return DiscoveredLeader(name: name, endpoint: result.endpoint)
            }
            DispatchQueue.main.async { self?.updateDiscovered(leaders) }
        }
        browser.stateUpdateHandler = { [weak self] state in
            guard case let .failed(error) = state else { return }
            DispatchQueue.main.async {
                self?.status = "Browse failed: \(error.localizedDescription)"
                self?.append("Browse failed: \(error.localizedDescription)")
            }
        }
        self.browser = browser
        browser.start(queue: .main)
    }

    private func updateDiscovered(_ leaders: [DiscoveredLeader]) {
        discoveredLeaders = leaders.sorted { $0.name < $1.name }
        if connectedLeader == nil {
            status = leaders.isEmpty ? "Looking for leaders…" : "Pick a leader"
        }
    }

    private func audioClientStateChanged(_ state: AudioStreamClient.State) {
        switch state {
        case .connected:
            append("Audio stream connected")
        case let .reconnecting(reason):
            append("Audio stream reconnecting: \(reason)")
        case .connecting, .stopped:
            break
        }
    }

    private func followerStateChanged(_ state: ClockSyncFollower.State) {
        switch state {
        case .connecting:
            status = "Connecting to \(connectedLeader ?? "leader")…"
        case .syncing:
            status = "Syncing with \(connectedLeader ?? "leader")"
            append("Connected")
        case let .reconnecting(reason):
            status = "Reconnecting to \(connectedLeader ?? "leader")…"
            append("Reconnecting: \(reason)")
        case let .failed(message):
            status = "Failed: \(message)"
            append("Follower error: \(message)")
        case .stopped:
            break
        }
    }

    private func record(_ round: ClockSyncRound) {
        latestRound = round
        player.update(clockMap: round.map)
        rounds.append(round)
        if rounds.count > historyLimit {
            rounds.removeFirst(rounds.count - historyLimit)
        }

        let rtt = round.best.map { String(format: "rtt %.2fms", Double($0.roundTrip) / 1e6) } ?? "no replies"
        var line = "#\(round.index) \(round.probesReceived)/\(round.probesSent) \(rtt) \(round.verdict)"
        if let map = round.map {
            let offset = Double(map.offset(atLocal: round.localTime)) / 1e6
            line += String(format: " offset %.3fms skew %+.2fppm", offset, map.skewPPM)
        }
        if let residual = round.residual {
            line += String(format: " res %+.0fµs", Double(residual) / 1e3)
        }
        append(line)
    }

    // MARK: - Helpers

    private func tearDown() {
        player.update(playback: .stopped)
        player.update(clockMap: nil)
        player.onOutgoingStream = nil
        audioServer?.stop()
        audioServer = nil
        audioClient?.stop()
        audioClient = nil
        leader?.stop()
        leader = nil
        follower?.stop()
        follower = nil
        browser?.cancel()
        browser = nil
        followerCount = 0
        discoveredLeaders = []
        connectedLeader = nil
        latestRound = nil
        rounds = []
    }

    private func append(_ text: String) {
        log.append(LogLine(date: .now, text: text))
        if log.count > logLimit {
            log.removeFirst(log.count - logLimit)
        }
        ClockSyncNetwork.logger.debug("\(text, privacy: .public)")
    }

    private static func makeDeviceName() -> String {
        #if os(macOS)
        let base = Host.current().localizedName ?? "Mac"
        #else
        let base = UIDevice.current.model
        #endif
        let suffix = String(UInt16.random(in: .min ... .max), radix: 16, uppercase: true)
        return "\(base) \(suffix)"
    }
}
