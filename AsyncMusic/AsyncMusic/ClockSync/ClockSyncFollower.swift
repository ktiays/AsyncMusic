//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network
import os

/// The result of one probe round, as reported to the UI.
nonisolated struct ClockSyncRound: Sendable {
    var index: Int
    /// Follower host time at which the round finished.
    var localTime: Int64
    var verdict: ClockVerdict
    var probesSent: Int
    var probesReceived: Int
    /// The fastest probe of the round, if any reply arrived.
    var best: ClockProbe?
    /// How far `best` lies from the map fitted after this round, in nanoseconds.
    var residual: Int64?
    var map: ClockMap?
    var minimumRoundTrip: Int64?
    var residualRMS: Double
    var acceptedCount: Int
    var rejectedCount: Int
    var windowCount: Int
}

/// Probes one leader in rounds and keeps a `ClockMap` from this device's host clock to the
/// leader's.
///
/// Each round sends a short burst of pings. Spacing them out decorrelates their delays, and
/// the first one also wakes the Wi-Fi radio out of power save so the later ones see the
/// fast path. Only the round's fastest exchange is fed to the estimator. After each round the
/// follower says hello (its device ID and name), which tells the leader it is still there and
/// which speaker channel to give it.
///
/// When the connection fails or the leader stops answering (the app was suspended, Wi-Fi
/// dropped, the leader's listener came back on a new port), a fresh connection to the same
/// Bonjour service is opened. The estimate is kept across reconnects, since the leader's clock
/// did not change; it is only discarded if the leader reports a new session.
nonisolated final class ClockSyncFollower: @unchecked Sendable {
    struct Configuration: Sendable {
        var probesPerRound = 8
        var probeSpacing: DispatchTimeInterval = .milliseconds(15)
        /// How long to wait for stragglers after the last ping of a round.
        var replyGrace: DispatchTimeInterval = .milliseconds(250)
        /// Rounds run faster at first so a usable offset is available within a few seconds.
        var warmupRounds = 10
        var warmupInterval: DispatchTimeInterval = .milliseconds(300)
        var steadyInterval: DispatchTimeInterval = .seconds(1)
        /// Reconnect after this many consecutive rounds without a single reply.
        var silentRoundsBeforeReconnect = 5
        var reconnectDelay: DispatchTimeInterval = .seconds(1)
    }

    enum State: Sendable, Equatable {
        case connecting
        case syncing
        case reconnecting(String)
        case failed(String)
        case stopped
    }

    let configuration: Configuration

    var onStateChange: (@Sendable (State) -> Void)?
    var onRound: (@Sendable (ClockSyncRound) -> Void)?
    /// Called when the leader's playback state changes (not for its periodic repeats).
    var onPlayback: (@Sendable (PlaybackState) -> Void)?
    /// Called when the leader's audio stream endpoint becomes known or changes: the address
    /// this connection reached, with the port the leader announced.
    var onAudioEndpoint: (@Sendable (NWEndpoint) -> Void)?
    /// Called when the leader assigns this device a speaker channel (not for repeats).
    var onSpeakers: (@Sendable (_ layout: SpeakerLayout, _ channel: SpeakerChannel, _ switchLeaderTime: Int64) -> Void)?

    private let queue = DispatchQueue(label: "me.ktiays.AsyncMusic.clock-sync.follower", qos: .userInteractive)
    private let endpoint: NWEndpoint
    private let identity: DeviceIdentity
    private var connection: NWConnection?
    /// Pings are only sent on a ready connection; one queued earlier would carry a stale t0.
    private var connectionReady = false
    private var silentRounds = 0
    private var estimator = ClockEstimator(asyncmusic.ClockEstimatorConfiguration())
    /// True from `start()` until `stop()`, across reconnects.
    private var running = false
    private var roundIndex = 0
    private var roundProbes: [ClockProbe] = []
    private var roundProbesSent = 0
    private var leaderSession: UInt64?
    private var playbackRevision: (session: UInt64, revision: UInt32)?
    private var speakersRevision: (session: UInt64, revision: UInt32)?
    private var lastSleepDuration = HostClock.sleepDuration()
    private var audioEndpoint: NWEndpoint?

    init(endpoint: NWEndpoint, identity: DeviceIdentity, configuration: Configuration = Configuration()) {
        self.configuration = configuration
        self.endpoint = endpoint
        self.identity = identity
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true
            onStateChange?(.connecting)
            connect()
            startRound()
        }
    }

    func stop() {
        queue.async { [self] in
            running = false
            connectionReady = false
            connection?.cancel()
            connection = nil
            onStateChange?(.stopped)
        }
    }

    private func connect() {
        guard running else { return }
        let connection = NWConnection(to: endpoint, using: ClockSyncNetwork.parameters())
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self, let connection, connection === self.connection else { return }
            connectionStateChanged(state)
        }
        self.connection = connection
        connectionReady = false
        silentRounds = 0
        connection.start(queue: queue)
    }

    private func reconnect(reason: String) {
        guard running else { return }
        ClockSyncNetwork.logger.info("Follower reconnecting: \(reason, privacy: .public)")
        onStateChange?(.reconnecting(reason))
        connectionReady = false
        connection?.cancel()
        connection = nil
        queue.asyncAfter(deadline: .now() + configuration.reconnectDelay) { [self] in
            if connection == nil {
                connect()
            }
        }
    }

    private func connectionStateChanged(_ state: NWConnection.State) {
        switch state {
        case .ready:
            guard let connection, !connectionReady else { return }
            connectionReady = true
            onStateChange?(.syncing)
            receive(on: connection)
            sendHello()
        case let .waiting(error):
            // The connection retries by itself once a usable network path appears.
            connectionReady = false
            onStateChange?(.reconnecting("Waiting: \(error.localizedDescription)"))
        case let .failed(error):
            ClockSyncNetwork.logger.error("Follower connection failed: \(error, privacy: .public)")
            reconnect(reason: error.localizedDescription)
        default:
            break
        }
    }

    private func startRound() {
        guard running else { return }
        // If this device slept, its host clock stood still while the leader's ran on, so the
        // whole estimate is off by the sleep time. Start over (and assume the connection died).
        let slept = HostClock.sleepDuration()
        if slept - lastSleepDuration > HostClock.sleepDetectionThreshold {
            lastSleepDuration = slept
            estimator.reset()
            reconnect(reason: "Device slept")
        }
        roundIndex += 1
        roundProbes = []
        roundProbesSent = 0
        let round = roundIndex

        for index in 0 ..< configuration.probesPerRound {
            queue.asyncAfter(deadline: .now() + configuration.probeSpacing * index) { [self] in
                guard running, roundIndex == round, connectionReady, let connection else { return }
                let sequence = UInt32(truncatingIfNeeded: round) << 8 | UInt32(index)
                // Stamp right before handing the datagram to the network stack.
                let ping = ClockSyncMessage.ping(sequence: sequence, t0: HostClock.now())
                connection.send(content: ping.encoded(), completion: .idempotent)
                roundProbesSent += 1
            }
        }

        let roundLength = configuration.probeSpacing * (configuration.probesPerRound - 1)
        queue.asyncAfter(deadline: .now() + roundLength + configuration.replyGrace) { [self] in
            finishRound(round)
        }
    }

    private func finishRound(_ round: Int) {
        guard running, roundIndex == round else { return }
        let verdict = estimator.addRound(roundProbes)
        if connectionReady, roundProbesSent > 0, roundProbes.isEmpty {
            silentRounds += 1
            if silentRounds >= configuration.silentRoundsBeforeReconnect {
                reconnect(reason: "Leader stopped answering")
            }
        } else if !roundProbes.isEmpty {
            silentRounds = 0
        }
        let best = roundProbes.min { $0.roundTrip() < $1.roundTrip() }
        let map = estimator.fittedMap
        let residual = best.flatMap { best in
            map.map { best.offset() - $0.offset(atLocal: best.localMidpoint()) }
        }

        onRound?(ClockSyncRound(
            index: round,
            localTime: HostClock.now(),
            verdict: verdict,
            probesSent: roundProbesSent,
            probesReceived: roundProbes.count,
            best: best,
            residual: residual,
            map: map,
            minimumRoundTrip: estimator.lowestRoundTrip,
            residualRMS: estimator.residualRMS(),
            acceptedCount: Int(estimator.acceptedCount()),
            rejectedCount: Int(estimator.rejectedCount()),
            windowCount: Int(estimator.windowCount())
        ))

        sendHello()

        let interval = round < configuration.warmupRounds ? configuration.warmupInterval : configuration.steadyInterval
        queue.asyncAfter(deadline: .now() + interval) { [self] in
            startRound()
        }
    }

    private func receive(on connection: NWConnection) {
        connection.receiveMessage { [weak self, weak connection] content, _, _, error in
            // Stamp before doing anything else, mirroring the leader's t1.
            let t3 = HostClock.now()
            guard let self, let connection, connection === self.connection else { return }
            switch content.flatMap(ClockSyncMessage.init) {
            case let .pong(sequence, session, t0, t1, t2):
                handlePong(sequence: sequence, session: session, probe: ClockProbe(t0: t0, t1: t1, t2: t2, t3: t3))
            case let .playback(revision, session, state):
                handlePlayback(revision: revision, session: session, state: state)
            case let .audioPort(_, port):
                handleAudioPort(port, connection: connection)
            case let .speakers(revision, session, layout, channel, switchLeaderTime):
                handleSpeakers(revision: revision, session: session, layout: layout, channel: channel, switchLeaderTime: switchLeaderTime)
            case .ping, .hello, nil:
                break
            }
            if error == nil, running {
                receive(on: connection)
            }
        }
    }

    private func handleAudioPort(_ port: UInt16, connection: NWConnection) {
        guard case let .hostPort(host, _)? = connection.currentPath?.remoteEndpoint,
              let port = NWEndpoint.Port(rawValue: port)
        else { return }
        let endpoint = NWEndpoint.hostPort(host: host, port: port)
        guard endpoint != audioEndpoint else { return }
        audioEndpoint = endpoint
        onAudioEndpoint?(endpoint)
    }

    /// Sent outside the probe bursts so it never delays a ping.
    private func sendHello() {
        guard connectionReady, let connection else { return }
        connection.send(content: ClockSyncMessage.hello(device: identity.id, name: identity.name).encoded(), completion: .idempotent)
    }

    private func handleSpeakers(revision: UInt32, session: UInt64, layout: SpeakerLayout, channel: SpeakerChannel, switchLeaderTime: Int64) {
        if let current = speakersRevision, current.session == session,
           Int32(bitPattern: revision &- current.revision) <= 0 {
            return
        }
        speakersRevision = (session, revision)
        onSpeakers?(layout, channel, switchLeaderTime)
    }

    private func handlePlayback(revision: UInt32, session: UInt64, state: PlaybackState) {
        // Ignore repeats, and older states that UDP delivered out of order.
        if let current = playbackRevision, current.session == session,
           Int32(bitPattern: revision &- current.revision) <= 0 {
            return
        }
        playbackRevision = (session, revision)
        onPlayback?(state)
    }

    private func handlePong(sequence: UInt32, session: UInt64, probe: ClockProbe) {
        if leaderSession != session {
            if leaderSession != nil {
                ClockSyncNetwork.logger.info("Leader session changed; discarding clock estimate")
            }
            leaderSession = session
            estimator.reset()
        }
        // Late replies from an earlier round would skew this round's minimum; drop them.
        guard sequence >> 8 == UInt32(truncatingIfNeeded: roundIndex) & 0xFF_FFFF else { return }
        roundProbes.append(probe)
    }
}

private nonisolated func * (lhs: DispatchTimeInterval, rhs: Int) -> DispatchTimeInterval {
    switch lhs {
    case let .seconds(value): .seconds(value * rhs)
    case let .milliseconds(value): .milliseconds(value * rhs)
    case let .microseconds(value): .microseconds(value * rhs)
    case let .nanoseconds(value): .nanoseconds(value * rhs)
    case .never: .never
    @unknown default: .never
    }
}
