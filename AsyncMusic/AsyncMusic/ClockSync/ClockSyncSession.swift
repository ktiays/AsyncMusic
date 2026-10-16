//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network
import Observation
import os

/// Drives clock sync and synced playback for the UI: either leads (advertises its clock and
/// decides what plays) or follows a discovered leader, and keeps a short history of rounds for
/// the diagnostics view.
///
/// The leader also decides how the music is spread over the group (`SpeakerLayout`): every
/// device playing the full mix, or a stereo pair with one device per channel. Channels are
/// assigned to devices by their stable ID and remembered, so the same two devices pair up
/// again next time. While a channel has no device present, everyone plays the full mix.
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

    let identity = DeviceIdentity.current()
    var deviceName: String { identity.name }
    let player = SyncedPlayer()

    /// How far ahead the leader schedules a start. Must comfortably exceed the time for the
    /// command to reach followers plus the largest output latency in the group (Bluetooth can
    /// be 200+ ms). Followers that hear about a start late just join mid-stream.
    let startLeadTime: Int64 = 1_000_000_000
    /// Files are first streamed to every follower; playback starts once each has
    /// `startBufferDuration` buffered (adaptive start), but waits at most this long.
    let maximumStartWait = 3.0
    let startBufferDuration = 0.5
    /// Once everyone is ready, the start is scheduled this far ahead, covering the play
    /// command's trip to followers and their output latency.
    let startMargin: Int64 = 500_000_000
    /// Channel changes take effect this far ahead, so every device switches at the same moment.
    let channelSwitchLead: Int64 = 400_000_000

    private(set) var isLoadingTrack = false
    /// The file the leader plays with "Play file".
    private(set) var selectedTrack: MediaFile?

    private(set) var role: Role = .off
    private(set) var status = "Idle"

    // Leader
    private(set) var followerCount = 0
    /// Followers that introduced themselves, with whether each is currently reachable.
    private(set) var members: [ClockSyncLeader.Member] = []
    private(set) var speakerLayout: SpeakerLayout
    /// The device chosen for each channel of the layout. Kept per channel across layouts and
    /// launches; entries for devices not around are kept too, in case they come back.
    private(set) var channelAssignments: [SpeakerChannel: UInt64]
    /// The channel each device plays right now (absent: the full mix), as sent to the group.
    private(set) var activeChannels: [UInt64: SpeakerChannel] = [:]

    /// True when the system reports that this app may not use the local network, which
    /// silently breaks most of the group traffic. The user has to allow it in Settings.
    private(set) var isLocalNetworkDenied = false

    // Follower
    private(set) var discoveredLeaders: [DiscoveredLeader] = []
    private(set) var connectedLeader: String?
    private(set) var followerState: ClockSyncFollower.State?
    private(set) var latestRound: ClockSyncRound?
    private(set) var rounds: [ClockSyncRound] = []
    /// The layout the leader announced; this device's channel is `player.channel`.
    private(set) var remoteLayout = SpeakerLayout.everywhere

    private(set) var log: [LogLine] = []

    /// The current mapping to the leader's clock; identity when leading.
    var clockMap: ClockMap? {
        switch role {
        case .leader: .identity()
        case .follower: latestRound?.map
        case .off: nil
        }
    }

    /// True while a file is being buffered on followers before it starts.
    var isPreparingPlayback: Bool { pendingStart != nil }

    /// Whether every channel of the layout has a device that is present.
    var isLayoutComplete: Bool {
        speakerLayout.channels.allSatisfy { channel in
            channelAssignments[channel].map(isDevicePresent) ?? false
        }
    }

    private let historyLimit = 300
    private let logLimit = 200

    private var leader: ClockSyncLeader?
    private var follower: ClockSyncFollower?
    private var audioServer: AudioStreamServer?
    /// The stream the leader is currently sending, as reported by the player.
    private var outgoingStream: OutgoingStream?
    /// Identifies the play request whose adaptive start is pending, so a newer request or a
    /// stop cancels it.
    private var pendingStart: UUID?
    private var endOfTrackTask: Task<Void, Never>?
    /// The layout last sent to followers, so a layout change is announced even if no device's
    /// channel changed (e.g. a stereo pair still waiting for its second device).
    private var leaderAnnouncedLayout: SpeakerLayout?
    private var audioClient: AudioStreamClient?
    private var browser: NWBrowser?

    private static let layoutKey = "ClockSyncSession.speakerLayout"
    private static let assignmentsKey = "ClockSyncSession.channelAssignments"

    init() {
        let defaults = UserDefaults.standard
        speakerLayout = SpeakerLayout(rawValue: UInt8(clamping: defaults.integer(forKey: Self.layoutKey))) ?? .everywhere
        var assignments: [SpeakerChannel: UInt64] = [:]
        for (key, value) in defaults.dictionary(forKey: Self.assignmentsKey) ?? [:] {
            if let raw = UInt8(key), let channel = SpeakerChannel(rawValue: raw), let device = (value as? NSNumber)?.uint64Value {
                assignments[channel] = device
            }
        }
        channelAssignments = assignments
    }

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
            player.update(clockMap: .identity())
            startLeading()
            startBrowsing()
        case .follower:
            startBrowsing()
        }
    }

    func follow(_ discovered: DiscoveredLeader) {
        follower?.stop()
        rounds = []
        latestRound = nil
        player.update(clockMap: nil)
        player.update(playback: .stopped())
        connectedLeader = discovered.name
        followerState = nil
        append("Following \(discovered.name)")

        resetRemoteSpeakers()

        let follower = ClockSyncFollower(endpoint: discovered.endpoint, identity: identity)
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
        follower.onSpeakers = { [weak self] layout, channel, switchTime in
            DispatchQueue.main.async { self?.applyRemoteSpeakers(layout: layout, channel: channel, switchTime: switchTime) }
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

    /// Leader only: starts `source` from the top on every device. The metronome starts after
    /// `startLeadTime`; files start as soon as every follower has buffered enough.
    func play(source: UInt64) {
        guard role == .leader, let leader else { return }
        pendingStart = nil
        guard !PlaybackSource.isGenerated(source) else {
            let state = PlaybackState(source: source, startLeaderTime: HostClock.now() + startLeadTime, startMediaTime: 0)
            player.update(playback: state)
            leader.setPlayback(state)
            append("Play \(describe(source)) in \(Double(startLeadTime) / 1e9)s")
            return
        }

        // Start decoding (and with it streaming) right away, provisionally scheduled after the
        // longest wait. Followers buffer the stream without being told to play yet.
        let requested = HostClock.now()
        let provisional = PlaybackState(
            source: source,
            startLeaderTime: requested + Int64(maximumStartWait * 1e9) + startMargin,
            startMediaTime: 0
        )
        player.update(playback: provisional)
        guard let stream = outgoingStream, stream.source == source, let audioServer else {
            leader.setPlayback(provisional)
            scheduleEndOfTrack(provisional)
            return
        }
        let token = UUID()
        pendingStart = token
        append("Buffering \(describe(source)) on followers…")

        let threshold = stream.buffer.validRange().lowerBound + Int64(startBufferDuration * stream.buffer.sampleRate())
        audioServer.waitUntilBuffered(stream: stream.id, through: threshold, timeout: maximumStartWait) { [weak self] readiness in
            DispatchQueue.main.async {
                guard let self, self.pendingStart == token, let leader = self.leader else { return }
                self.pendingStart = nil
                let state = PlaybackState(source: source, startLeaderTime: HostClock.now() + self.startMargin, startMediaTime: 0)
                self.player.update(playback: state)
                leader.setPlayback(state)
                self.scheduleEndOfTrack(state)
                let total = Double(state.startLeaderTime - requested) / 1e9
                var line = String(format: "Play %@: %d/%d followers ready, starts %.2fs after request", self.describe(source), readiness.ready, readiness.total, total)
                if readiness.timedOut {
                    line += " (gave up waiting)"
                }
                self.append(line)
            }
        }
    }

    /// Leader only: stops playback on every device.
    func stopPlayback() {
        guard role == .leader, let leader else { return }
        pendingStart = nil
        endOfTrackTask?.cancel()
        leader.setPlayback(.stopped())
        player.update(playback: .stopped())
        append("Stop")
    }

    /// Stops the group once the file in `state` has played to its end.
    private func scheduleEndOfTrack(_ state: PlaybackState) {
        endOfTrackTask?.cancel()
        guard let file = player.files[state.source] else { return }
        // The leader's own clock is the group's timeline.
        let end = state.startLeaderTime - state.startMediaTime + Int64(file.duration * 1e9)
        endOfTrackTask = Task { [weak self] in
            try? await Task.sleep(for: .nanoseconds(max(0, end - HostClock.now())) + .milliseconds(300))
            guard !Task.isCancelled, let self, self.role == .leader, self.player.playback == state else { return }
            self.append("Finished \(self.describe(state.source))")
            self.stopPlayback()
        }
    }

    /// Reads a file's format and makes it available for playback. Followers get it streamed
    /// (or play their own copy, matched by content, if they loaded the same file).
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
        case PlaybackSource.channelTest: "channel test"
        default: player.files[source].map { "“\($0.name)”" } ?? String(format: "streamed file %016llx", source)
        }
    }

    private func applyRemotePlayback(_ state: PlaybackState) {
        guard role == .follower else { return }
        player.update(playback: state)
        if state.isPlaying() {
            let inSeconds = Double(state.startLeaderTime - (clockMap?.leaderTime(fromLocal: HostClock.now()) ?? state.startLeaderTime)) / 1e9
            append(String(format: "Leader plays %@ (start in %.3fs)", describe(state.source), inSeconds))
        } else {
            append("Leader stopped")
        }
    }

    // MARK: - Speakers

    /// Leader only: spreads the music over the group as `layout` describes.
    func setSpeakerLayout(_ layout: SpeakerLayout) {
        guard layout != speakerLayout else { return }
        speakerLayout = layout
        UserDefaults.standard.set(Int(layout.rawValue), forKey: Self.layoutKey)
        append("Speaker layout: \(layout.title)")
        updateSpeakers()
    }

    /// Leader only: plays `channel` on `device` (nil: nobody). A device already playing another
    /// channel trades places with the one it replaces.
    func assign(_ device: UInt64?, to channel: SpeakerChannel) {
        var assignments = channelAssignments
        let previous = assignments[channel]
        if let device, let other = assignments.first(where: { $0.value == device && $0.key != channel })?.key {
            assignments[other] = previous
        }
        assignments[channel] = device
        setAssignments(assignments)
    }

    /// Leader only: swaps the devices of a two-channel layout (left and right of a stereo pair).
    func swapChannels() {
        let channels = speakerLayout.channels
        guard channels.count == 2 else { return }
        var assignments = channelAssignments
        (assignments[channels[0]], assignments[channels[1]]) = (assignments[channels[1]], assignments[channels[0]])
        setAssignments(assignments)
    }

    func name(ofDevice id: UInt64) -> String? {
        id == identity.id ? deviceName : members.first { $0.id == id }?.name
    }

    /// This device, or a follower that is reachable now.
    func isDevicePresent(_ id: UInt64) -> Bool {
        id == identity.id || members.contains { $0.id == id && $0.isActive }
    }

    private func setAssignments(_ assignments: [SpeakerChannel: UInt64]) {
        guard assignments != channelAssignments else { return }
        storeAssignments(assignments)
        updateSpeakers()
    }

    private func storeAssignments(_ assignments: [SpeakerChannel: UInt64]) {
        channelAssignments = assignments
        let stored = Dictionary(uniqueKeysWithValues: assignments.map { (String($0.key.rawValue), NSNumber(value: $0.value)) })
        UserDefaults.standard.set(stored, forKey: Self.assignmentsKey)
    }

    /// Fills channels that have no device in the group with the obvious choice: this device
    /// first, then a follower if exactly one is left over. That pairs the common two-device
    /// setup without asking, and keeps a pairing once made.
    private func fillOpenChannels() {
        var assignments = channelAssignments
        let known = Set(members.map(\.id)).union([identity.id])
        var open = speakerLayout.channels.filter { assignments[$0].map { !known.contains($0) } ?? true }
        guard !open.isEmpty else { return }
        let assigned = Set(speakerLayout.channels.compactMap { assignments[$0] })
        if !assigned.contains(identity.id) {
            assignments[open.removeFirst()] = identity.id
        }
        let spare = members.filter { $0.isActive && !assigned.contains($0.id) }
        if open.count == 1, spare.count == 1 {
            assignments[open[0]] = spare[0].id
            append("Paired \(spare[0].name) as the \(open[0].title.lowercased()) speaker")
        }
        if assignments != channelAssignments {
            storeAssignments(assignments)
        }
    }

    /// Works out every device's channel and, if anything changed, tells the group. The switch
    /// is scheduled slightly ahead so all devices make it at the same instant.
    private func updateSpeakers() {
        guard role == .leader, let leader else { return }
        if !speakerLayout.channels.isEmpty, !isLayoutComplete {
            fillOpenChannels()
        }
        var channels: [UInt64: SpeakerChannel] = [:]
        if isLayoutComplete {
            for channel in speakerLayout.channels {
                if let device = channelAssignments[channel] {
                    channels[device] = channel
                }
            }
        }
        guard channels != activeChannels || leaderAnnouncedLayout != speakerLayout else { return }
        activeChannels = channels
        leaderAnnouncedLayout = speakerLayout
        let switchTime = HostClock.now() + channelSwitchLead
        leader.setSpeakers(layout: speakerLayout, channels: channels, switchTime: switchTime)
        player.setChannel(channels[identity.id] ?? .full, at: switchTime)
        if !speakerLayout.channels.isEmpty {
            let summary = speakerLayout.channels.map { "\($0.title): \(channelAssignments[$0].flatMap(name(ofDevice:)) ?? "–")" }
            append(channels.isEmpty ? "Not all speakers present; everyone plays the full mix" : "Speakers: " + summary.joined(separator: ", "))
        }
    }

    private func applyRemoteSpeakers(layout: SpeakerLayout, channel: SpeakerChannel, switchTime: Int64) {
        guard role == .follower else { return }
        remoteLayout = layout
        if channel != player.channel {
            append("This device now plays \(channel.title.lowercased())")
        }
        player.setChannel(channel, at: switchTime)
    }

    private func resetRemoteSpeakers() {
        remoteLayout = .everywhere
        player.setChannel(.full)
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
                self.player.update(playback: .stopped())
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
        leader.onMembersChange = { [weak self] members in
            DispatchQueue.main.async {
                guard let self, self.role == .leader else { return }
                self.members = members
                self.updateSpeakers()
            }
        }
        self.leader = leader
        leader.start()
        updateSpeakers()

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
        player.onOutgoingStream = { [weak self, weak server] stream in
            server?.setStream(stream)
            self?.outgoingStream = stream
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

    /// Browses for leaders. A leader browses too, ignoring the results: browsing makes the
    /// system ask for the Local Network privilege if it has not yet, and its state is the only
    /// way to tell that the privilege was denied.
    private func startBrowsing() {
        let isFollower = role == .follower
        if isFollower {
            status = "Looking for leaders…"
        }
        let browser = NWBrowser(for: .bonjour(type: ClockSyncNetwork.serviceType, domain: nil), using: ClockSyncNetwork.parameters())
        if isFollower {
            browser.browseResultsChangedHandler = { [weak self] results, _ in
                let leaders = results.compactMap { result -> DiscoveredLeader? in
                    guard case let .service(name, _, _, _) = result.endpoint else { return nil }
                    return DiscoveredLeader(name: name, endpoint: result.endpoint)
                }
                DispatchQueue.main.async { self?.updateDiscovered(leaders) }
            }
        }
        browser.stateUpdateHandler = { [weak self] state in
            DispatchQueue.main.async { self?.browserStateChanged(state) }
        }
        self.browser = browser
        browser.start(queue: .main)
    }

    private func browserStateChanged(_ state: NWBrowser.State) {
        switch state {
        case .ready:
            if isLocalNetworkDenied {
                append("Local network access granted")
            }
            isLocalNetworkDenied = false
        case let .waiting(error):
            // kDNSServiceErr_PolicyDenied: the user turned off Local Network access for this app.
            if case let .dns(code) = error, code == -65570 {
                if !isLocalNetworkDenied {
                    append("Local network access is denied; other devices cannot hear this one")
                }
                isLocalNetworkDenied = true
            }
        case let .failed(error):
            if role == .follower {
                status = "Browse failed: \(error.localizedDescription)"
            }
            append("Browse failed: \(error.localizedDescription)")
        default:
            break
        }
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
        followerState = state
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

        let rtt = round.best.map { String(format: "rtt %.2fms", Double($0.roundTrip()) / 1e6) } ?? "no replies"
        var line = "#\(round.index) \(round.probesReceived)/\(round.probesSent) \(rtt) \(round.verdict.name)"
        if let map = round.map {
            let offset = Double(map.offset(atLocal: round.localTime)) / 1e6
            line += String(format: " offset %.3fms skew %+.2fppm", offset, map.skewPPM())
        }
        if let residual = round.residual {
            line += String(format: " res %+.0fµs", Double(residual) / 1e3)
        }
        append(line)
    }

    // MARK: - Helpers

    private func tearDown() {
        player.update(playback: .stopped())
        player.update(clockMap: nil)
        player.onOutgoingStream = nil
        outgoingStream = nil
        pendingStart = nil
        endOfTrackTask?.cancel()
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
        isLocalNetworkDenied = false
        followerCount = 0
        members = []
        activeChannels = [:]
        leaderAnnouncedLayout = nil
        resetRemoteSpeakers()
        discoveredLeaders = []
        connectedLeader = nil
        followerState = nil
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
}
