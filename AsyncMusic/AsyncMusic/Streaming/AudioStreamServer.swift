//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import Network
import os

/// What the leader is currently playing from a file: its own ring buffer, which the server
/// forwards to followers frame for frame.
nonisolated struct OutgoingStream: @unchecked Sendable {
    let id: UInt64
    let source: UInt64
    let buffer: StreamBuffer
    let playback: PlaybackState
}

/// Leader side of audio streaming: accepts one TCP connection per follower and forwards the
/// leader's own decoded stream to each.
///
/// The listener is not advertised over Bonjour. Followers learn its port through the clock
/// sync channel (`onPortChange` → `ClockSyncLeader.setAudioPort`) and connect to the address
/// they already sync with, which avoids a second service type and lookup.
///
/// Nothing is decoded twice and no extra pacing is needed: the leader's feeder already keeps
/// its buffer a few seconds ahead of playback, so each sender just forwards whatever is in
/// the buffer that it has not sent yet. That means a burst on start (which fills followers'
/// buffers within the startup delay) and then a trickle at the playback rate. A follower
/// joining mid-stream gets audio from the currently playing position.
nonisolated final class AudioStreamServer: @unchecked Sendable {
    var onPortChange: (@Sendable (UInt16?) -> Void)?
    var onReceiversChange: (@Sendable (Int) -> Void)?
    var onError: (@Sendable (String) -> Void)?

    private let queue = DispatchQueue(label: "me.ktiays.AsyncMusic.audio-stream.server", qos: .userInitiated)
    private var listener: NWListener?
    private var senders: [ObjectIdentifier: Sender] = [:]
    private var stream: OutgoingStream?
    private var timer: DispatchSourceTimer?
    private var running = false

    static let logger = Logger(subsystem: "me.ktiays.AsyncMusic", category: "AudioStream")

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true
            startListener()
            // Picks up frames the feeder appended since the last send.
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now(), repeating: .milliseconds(20))
            timer.setEventHandler { [weak self] in
                self?.senders.values.forEach { $0.pump() }
            }
            timer.resume()
            self.timer = timer
        }
    }

    func stop() {
        queue.async { [self] in
            running = false
            timer?.cancel()
            timer = nil
            listener?.cancel()
            listener = nil
            onPortChange?(nil)
            senders.values.forEach { $0.connection.cancel() }
            senders = [:]
            onReceiversChange?(0)
        }
    }

    /// Switches every follower to `stream` (nil: nothing to send, e.g. metronome or stopped).
    func setStream(_ stream: OutgoingStream?) {
        queue.async { [self] in
            self.stream = stream
            senders.values.filter(\.isReady).forEach { $0.begin(stream) }
        }
    }

    private func startListener() {
        guard running, listener == nil else { return }
        do {
            let listener = try NWListener(using: AudioStreamNetwork.parameters())
            listener.stateUpdateHandler = { [weak self] state in
                guard let self else { return }
                if case .ready = state {
                    onPortChange?(self.listener?.port?.rawValue)
                } else if case let .failed(error) = state {
                    onPortChange?(nil)
                    Self.logger.error("Audio stream listener failed: \(error, privacy: .public)")
                    onError?(error.localizedDescription)
                    self.listener?.cancel()
                    self.listener = nil
                    queue.asyncAfter(deadline: .now() + .seconds(1)) { [weak self] in
                        self?.startListener()
                    }
                }
            }
            listener.newConnectionHandler = { [weak self] connection in
                self?.accept(connection)
            }
            self.listener = listener
            listener.start(queue: queue)
        } catch {
            onError?(error.localizedDescription)
        }
    }

    private func accept(_ connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        let sender = Sender(connection: connection, queue: queue)
        senders[id] = sender
        onReceiversChange?(senders.count)
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self else { return }
            switch state {
            case .ready:
                guard let sender = senders[id], !sender.isReady else { return }
                sender.isReady = true
                sender.begin(stream)
            case .failed, .cancelled:
                connection?.cancel()
                if senders.removeValue(forKey: id) != nil {
                    onReceiversChange?(senders.count)
                }
            default:
                break
            }
        }
        connection.start(queue: queue)
    }
}

/// Forwards one stream to one follower. Confined to the server's queue.
private nonisolated final class Sender: @unchecked Sendable {
    let connection: NWConnection
    var isReady = false

    private let queue: DispatchQueue
    private var stream: OutgoingStream?
    private var nextFrame: Int64 = 0
    private var inFlight = 0

    private let chunkFrames = 4096
    /// Sends handed to the network stack but not yet consumed by it. TCP's own send buffer
    /// sits behind this, so a slow follower backs up here instead of growing memory.
    private let maximumInFlight = 8
    private var scratch: [Int16]

    init(connection: NWConnection, queue: DispatchQueue) {
        self.connection = connection
        self.queue = queue
        scratch = [Int16](repeating: 0, count: chunkFrames * 2)
    }

    /// Announces `stream` and starts sending it from the frame being heard now (or from its
    /// first frame, if playback has not reached it yet).
    func begin(_ stream: OutgoingStream?) {
        self.stream = stream
        guard let stream else { return }
        let range = stream.buffer.validRange()
        let rate = stream.buffer.sampleRate
        let heard = stream.playback.mediaTime(atLeaderTime: HostClock.now())
        let heardFrame = Int64((Double(heard) * rate / 1e9).rounded(.down))
        // A little earlier than what is heard, to cover the follower's output latency.
        nextFrame = min(max(range.lowerBound, heardFrame - Int64(0.1 * rate)), range.upperBound)
        send(.streamStart(streamID: stream.id, source: stream.source, sampleRate: rate, startFrame: nextFrame))
        pump()
    }

    func pump() {
        guard isReady, let stream else { return }
        let buffer = stream.buffer
        while inFlight < maximumInFlight {
            let range = buffer.validRange()
            if nextFrame < range.lowerBound {
                // This follower fell so far behind that the frames it needs were overwritten.
                AudioStreamServer.logger.info("Receiver lapped; restarting its stream")
                begin(stream)
                return
            }
            let count = Int(min(Int64(chunkFrames), range.upperBound - nextFrame))
            guard count > 0 else { return }

            for offset in 0 ..< count {
                let (left, right) = buffer.sample(at: nextFrame + Int64(offset))
                scratch[offset * 2] = Self.pcm16(left)
                scratch[offset * 2 + 1] = Self.pcm16(right)
            }
            guard buffer.isStillValid(from: nextFrame) else { continue }

            let samples = scratch.withUnsafeBytes { Data($0.prefix(count * 4)) }
            send(.audio(streamID: stream.id, startFrame: nextFrame, frameCount: count, samples: samples))
            nextFrame += Int64(count)
        }
    }

    private func send(_ message: AudioStreamMessage) {
        inFlight += 1
        connection.send(content: message.framed(), completion: .contentProcessed { [weak self] _ in
            guard let self else { return }
            queue.async {
                self.inFlight -= 1
                self.pump()
            }
        })
    }

    @inline(__always)
    private static func pcm16(_ sample: Float) -> Int16 {
        // Scaling by 2^15 round-trips 16-bit sources exactly. Little-endian on every Apple
        // platform, so the array's bytes are the wire format.
        Int16(clamping: Int((sample * 32768).rounded()))
    }
}
