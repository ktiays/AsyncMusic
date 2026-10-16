//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network
import os

nonisolated enum ClockSyncNetwork {
    /// Must also be listed under `NSBonjourServices` in Info.plist.
    static let serviceType = "_asyncmusic._udp"

    static let logger = Logger(subsystem: "me.ktiays.AsyncMusic", category: "ClockSync")

    static func parameters() -> NWParameters {
        let parameters = NWParameters.udp
        // Marks packets for the Wi-Fi voice access category, which is queued ahead of bulk
        // traffic and noticeably tightens round-trip jitter.
        parameters.serviceClass = .interactiveVoice
        // Peer-to-peer (AWDL) works without a shared network, but its channel hopping adds
        // tens of milliseconds of jitter, so stick to infrastructure Wi-Fi / Ethernet.
        parameters.includePeerToPeer = false
        return parameters
    }
}

/// Advertises this device's host clock over Bonjour, answers every ping with a pong, and tells
/// followers what to play and which speaker channel each of them plays.
///
/// The leader keeps no per-follower sync state; all estimation happens on the followers. If the
/// listener fails (e.g. the network went away while the app was in the background) it is
/// recreated with the same session, so followers keep their clock estimate.
nonisolated final class ClockSyncLeader: @unchecked Sendable {
    enum State: Sendable, Equatable {
        case starting
        case advertising(port: UInt16)
        case failed(String)
        case stopped
    }

    /// A follower that introduced itself with a hello.
    struct Member: Sendable, Equatable, Identifiable {
        var id: UInt64
        var name: String
        /// Heard from within `activeWindow`. Members stay listed (inactive) until pruned.
        var isActive: Bool
    }

    /// Identifies the leader's timeline. It changes per run and whenever this device slept (its
    /// host clock stood still meanwhile), so followers know to discard their estimate.
    private var session = UInt64.random(in: .min ... .max)
    let name: String

    var onStateChange: (@Sendable (State) -> Void)?
    var onFollowersChange: (@Sendable (Int) -> Void)?
    /// Called whenever the set of members, or whether each is active, changes.
    var onMembersChange: (@Sendable ([Member]) -> Void)?
    /// Called when playback was stopped because this device slept and its clock jumped.
    var onPlaybackReset: (@Sendable () -> Void)?

    private let queue = DispatchQueue(label: "me.ktiays.AsyncMusic.clock-sync.leader", qos: .userInteractive)
    private var listener: NWListener?
    private var connections: [ObjectIdentifier: NWConnection] = [:]
    /// Host time each connection last received a packet. UDP flows never fail on their own, so
    /// this is how followers that went away are noticed.
    private var lastHeard: [ObjectIdentifier: Int64] = [:]
    private var running = false
    private var lastSleepDuration = HostClock.sleepDuration()
    private var playback = PlaybackState.stopped()
    private var playbackRevision: UInt32 = 0
    private var audioPort: UInt16?
    private var rebroadcastTimer: DispatchSourceTimer?
    /// The device behind each connection, from its hellos.
    private var devices: [ObjectIdentifier: (id: UInt64, name: String)] = [:]
    private var reportedMembers: [Member] = []
    private var layout = SpeakerLayout.everywhere
    private var channels: [UInt64: SpeakerChannel] = [:]
    private var channelSwitchTime: Int64 = 0
    private var speakersRevision: UInt32 = 0
    /// A follower silent for longer than this counts as gone for its speaker channel (it is
    /// only dropped altogether after 10 s). Followers ping at least once a second.
    private let activeWindow: Int64 = 3_000_000_000

    init(name: String) {
        self.name = name
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true
            onStateChange?(.starting)
            startListener()

            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + .seconds(1), repeating: .seconds(1))
            timer.setEventHandler { [weak self] in
                self?.checkForSleep()
                self?.pruneSilentFollowers()
                self?.broadcastPlayback()
                self?.reportMembers()
            }
            timer.resume()
            rebroadcastTimer = timer
        }
    }

    func stop() {
        queue.async { [self] in
            running = false
            listener?.cancel()
            listener = nil
            rebroadcastTimer?.cancel()
            rebroadcastTimer = nil
            for connection in connections.values {
                connection.cancel()
            }
            connections = [:]
            lastHeard = [:]
            devices = [:]
            reportMembers()
            onFollowersChange?(0)
            onStateChange?(.stopped)
        }
    }

    private func startListener() {
        guard running, listener == nil else { return }
        do {
            let listener = try NWListener(using: ClockSyncNetwork.parameters())
            listener.service = NWListener.Service(name: name, type: ClockSyncNetwork.serviceType)
            listener.stateUpdateHandler = { [weak self] state in
                self?.listenerStateChanged(state)
            }
            listener.newConnectionHandler = { [weak self] connection in
                self?.accept(connection)
            }
            self.listener = listener
            listener.start(queue: queue)
        } catch {
            onStateChange?(.failed(error.localizedDescription))
            scheduleListenerRestart()
        }
    }

    private func scheduleListenerRestart() {
        queue.asyncAfter(deadline: .now() + .seconds(1)) { [weak self] in
            self?.startListener()
        }
    }

    private func checkForSleep() {
        let slept = HostClock.sleepDuration()
        guard slept - lastSleepDuration > HostClock.sleepDetectionThreshold else { return }
        ClockSyncNetwork.logger.info("Leader device slept; starting a new session")
        lastSleepDuration = slept
        session = UInt64.random(in: .min ... .max)
        // Silence over the new timeline would be ambiguous; announce stopped playback.
        playback = .stopped()
        playbackRevision &+= 1
        onPlaybackReset?()
    }

    private func pruneSilentFollowers() {
        let cutoff = HostClock.now() - 10_000_000_000
        for (id, heard) in lastHeard where heard < cutoff {
            connections.removeValue(forKey: id)?.cancel()
            lastHeard.removeValue(forKey: id)
            devices.removeValue(forKey: id)
            onFollowersChange?(connections.count)
        }
    }

    /// Sends a new playback state to every follower now, and again every second until it changes.
    func setPlayback(_ state: PlaybackState) {
        queue.async { [self] in
            playback = state
            playbackRevision &+= 1
            broadcastPlayback()
        }
    }

    /// Tells followers where to connect for the audio stream (nil: not available).
    func setAudioPort(_ port: UInt16?) {
        queue.async { [self] in
            audioPort = port
            broadcastPlayback()
        }
    }

    /// Sets the group's speaker layout and each device's channel (devices not listed play the
    /// full mix), taking effect at leader time `switchTime`. Sent to every follower now and
    /// repeated every second until it changes.
    func setSpeakers(layout: SpeakerLayout, channels: [UInt64: SpeakerChannel], switchTime: Int64) {
        queue.async { [self] in
            self.layout = layout
            self.channels = channels
            channelSwitchTime = switchTime
            speakersRevision &+= 1
            for (id, connection) in connections {
                sendSpeakers(on: connection, id: id)
            }
        }
    }

    private func broadcastPlayback() {
        for (id, connection) in connections {
            sendPlayback(on: connection)
            sendSpeakers(on: connection, id: id)
        }
    }

    /// Sends the follower behind `connection` its channel, once it has said who it is.
    private func sendSpeakers(on connection: NWConnection, id: ObjectIdentifier) {
        guard let device = devices[id] else { return }
        let message = ClockSyncMessage.speakers(
            revision: speakersRevision,
            session: session,
            layout: layout,
            channel: channels[device.id] ?? .full,
            switchLeaderTime: channelSwitchTime
        )
        connection.send(content: message.encoded(), completion: .idempotent)
    }

    private func reportMembers() {
        let cutoff = HostClock.now() - activeWindow
        var members: [UInt64: Member] = [:]
        for (id, device) in devices {
            // A follower that reconnected briefly has two connections; either one counts.
            let isActive = (lastHeard[id] ?? .min) >= cutoff || members[device.id]?.isActive == true
            members[device.id] = Member(id: device.id, name: device.name, isActive: isActive)
        }
        let sorted = members.values.sorted { ($0.name, $0.id) < ($1.name, $1.id) }
        guard sorted != reportedMembers else { return }
        reportedMembers = sorted
        onMembersChange?(sorted)
    }

    /// Sends the playback state, and the audio stream's port along with it.
    private func sendPlayback(on connection: NWConnection) {
        let message = ClockSyncMessage.playback(revision: playbackRevision, session: session, state: playback)
        connection.send(content: message.encoded(), completion: .idempotent)
        if let audioPort {
            connection.send(content: ClockSyncMessage.audioPort(session: session, port: audioPort).encoded(), completion: .idempotent)
        }
    }

    private func listenerStateChanged(_ state: NWListener.State) {
        switch state {
        case .ready:
            let port = listener?.port?.rawValue ?? 0
            ClockSyncNetwork.logger.info("Leader advertising \(self.name, privacy: .public) on port \(port)")
            onStateChange?(.advertising(port: port))
        case let .failed(error):
            ClockSyncNetwork.logger.error("Leader listener failed: \(error, privacy: .public)")
            onStateChange?(.failed(error.localizedDescription))
            listener?.cancel()
            listener = nil
            scheduleListenerRestart()
        case let .waiting(error):
            onStateChange?(.failed("Waiting: \(error.localizedDescription)"))
        default:
            break
        }
    }

    private func accept(_ connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        connections[id] = connection
        lastHeard[id] = HostClock.now()
        onFollowersChange?(connections.count)
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self else { return }
            switch state {
            case .failed, .cancelled:
                connection?.cancel()
                lastHeard.removeValue(forKey: id)
                devices.removeValue(forKey: id)
                if connections.removeValue(forKey: id) != nil {
                    onFollowersChange?(connections.count)
                    reportMembers()
                }
            default:
                break
            }
        }
        connection.start(queue: queue)
        receive(on: connection)
        sendPlayback(on: connection)
    }

    private func receive(on connection: NWConnection) {
        connection.receiveMessage { [weak self, weak connection] content, _, _, error in
            // Stamp before doing anything else: any delay between the packet arriving and this
            // line shows up as path asymmetry.
            let t1 = HostClock.now()
            guard let self, let connection else { return }
            let id = ObjectIdentifier(connection)
            if connections[id] != nil {
                lastHeard[id] = t1
            }
            switch content.flatMap(ClockSyncMessage.init) {
            case let .ping(sequence, t0):
                // Stamp as late as possible; the turnaround (t2 - t1) is subtracted out exactly.
                let t2 = HostClock.now()
                let pong = ClockSyncMessage.pong(sequence: sequence, session: session, t0: t0, t1: t1, t2: t2)
                connection.send(content: pong.encoded(), completion: .idempotent)
            case let .hello(device, name):
                handleHello(device: device, name: name, connection: connection)
            default:
                break
            }
            if error == nil {
                receive(on: connection)
            }
        }
    }

    /// Answers every hello with the full state (playback, audio port, speaker channel).
    ///
    /// The periodic broadcast alone is not enough: on macOS, an app without the Local Network
    /// privilege may not send UDP to local addresses on its own, yet its replies to incoming
    /// datagrams (like pongs) still get through. Followers say hello every round, so answering
    /// them keeps every follower current within a second even then.
    private func handleHello(device: UInt64, name: String, connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        guard connections[id] != nil else { return }
        if devices[id]?.id != device || devices[id]?.name != name {
            devices[id] = (device, name)
            reportMembers()
        }
        sendPlayback(on: connection)
        sendSpeakers(on: connection, id: id)
    }
}
