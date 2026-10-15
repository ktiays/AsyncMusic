//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network
import os

/// What the leader is currently playing from a file: its own ring buffer, which the server
/// forwards to followers frame for frame.
nonisolated struct OutgoingStream: @unchecked Sendable {
    let id: UInt64
    let source: UInt64
    let buffer: StreamBuffer
    /// Where playback is, to start late joiners at the heard position.
    let position: PlaybackPosition
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
///
/// Followers report how far their buffer reaches, which `waitUntilBuffered` uses to start
/// playback as soon as everyone has enough (adaptive start) instead of after a fixed delay.
nonisolated final class AudioStreamServer: @unchecked Sendable {
    struct Readiness: Sendable {
        var ready: Int
        var total: Int
        var timedOut: Bool
    }

    private struct Waiter {
        var streamID: UInt64
        var frame: Int64
        var deadline: DispatchTime
        var completion: @Sendable (Readiness) -> Void
    }

    let codec: AudioStreamCodec

    var onPortChange: (@Sendable (UInt16?) -> Void)?
    var onReceiversChange: (@Sendable (Int) -> Void)?
    var onError: (@Sendable (String) -> Void)?

    private let queue = DispatchQueue(label: "me.ktiays.AsyncMusic.audio-stream.server", qos: .userInitiated)
    private var listener: NWListener?
    private var senders: [ObjectIdentifier: Sender] = [:]
    private var stream: OutgoingStream?
    private var timer: DispatchSourceTimer?
    private var running = false
    private var waiters: [Waiter] = []

    static let logger = Logger(subsystem: "me.ktiays.AsyncMusic", category: "AudioStream")

    init(codec: AudioStreamCodec = .alac) {
        self.codec = codec
    }

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
            checkWaiters()
        }
    }

    /// Switches every follower to `stream` (nil: nothing to send, e.g. metronome or stopped).
    func setStream(_ stream: OutgoingStream?) {
        queue.async { [self] in
            self.stream = stream
            senders.values.filter(\.isReady).forEach { $0.begin(stream) }
            checkWaiters()
        }
    }

    /// Calls `completion` (on the server's queue) once every connected follower has buffered
    /// `stream` up to `frame`, or after `timeout` seconds, whichever comes first. With no
    /// followers connected it completes right away.
    func waitUntilBuffered(stream streamID: UInt64, through frame: Int64, timeout: Double, completion: @escaping @Sendable (Readiness) -> Void) {
        queue.async { [self] in
            waiters.append(Waiter(streamID: streamID, frame: frame, deadline: .now() + timeout, completion: completion))
            checkWaiters()
            queue.asyncAfter(deadline: .now() + timeout) { [weak self] in
                self?.checkWaiters()
            }
        }
    }

    private func checkWaiters() {
        guard !waiters.isEmpty else { return }
        let now = DispatchTime.now()
        let receivers = senders.values.filter(\.isReady)
        waiters.removeAll { waiter in
            // Waiting for a stream that is no longer current is pointless.
            let current = stream?.id == waiter.streamID
            let ready = receivers.filter { $0.hasBuffered(stream: waiter.streamID, through: waiter.frame) }.count
            let timedOut = now >= waiter.deadline
            guard !current || ready == receivers.count || timedOut else { return false }
            waiter.completion(Readiness(ready: ready, total: receivers.count, timedOut: timedOut && ready < receivers.count))
            return true
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
        let sender = Sender(connection: connection, queue: queue, codec: codec)
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
                    checkWaiters()
                }
            default:
                break
            }
        }
        connection.start(queue: queue)
        connection.receiveAudioStreamMessages { [weak self] message in
            guard let self, case let .bufferReport(streamID, endFrame) = message else { return }
            senders[id]?.report(stream: streamID, endFrame: endFrame)
            checkWaiters()
        } onEnd: { [weak connection] _ in
            connection?.cancel()
        }
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
    private let codec: AudioStreamCodec
    private var encoder: ALACEncoder?
    /// The follower's last buffer report.
    private var reported: (streamID: UInt64, endFrame: Int64)?

    private let chunkFrames = ALACCodec.framesPerPacket
    /// Sends handed to the network stack but not yet consumed by it. TCP's own send buffer
    /// sits behind this, so a slow follower backs up here instead of growing memory.
    private let maximumInFlight = 8
    private var scratch: [Int16]

    init(connection: NWConnection, queue: DispatchQueue, codec: AudioStreamCodec) {
        self.connection = connection
        self.queue = queue
        self.codec = codec
        scratch = [Int16](repeating: 0, count: chunkFrames * 2)
    }

    /// Announces `stream` and starts sending it from the frame being heard now (or from its
    /// first frame, if playback has not reached it yet).
    func begin(_ stream: OutgoingStream?) {
        self.stream = stream
        guard let stream else { return }
        let range = stream.buffer.validRange()
        let rate = stream.buffer.sampleRate()
        let heardFrame = stream.position.heardMediaTime().map { Int64((Double($0) * rate / 1e9).rounded(.down)) } ?? range.lowerBound
        // A little earlier than what is heard, to cover the follower's output latency.
        nextFrame = min(max(range.lowerBound, heardFrame - Int64(0.1 * rate)), range.upperBound)

        var cookie = Data()
        if codec == .alac {
            if encoder == nil || encoder?.sampleRate != rate {
                encoder = ALACEncoder(sampleRate: rate)
            }
            cookie = encoder?.magicCookie ?? Data()
        }
        send(.streamStart(streamID: stream.id, source: stream.source, sampleRate: rate, startFrame: nextFrame, codec: encoder == nil ? .pcm16 : codec, cookie: cookie))
        pump()
    }

    func report(stream streamID: UInt64, endFrame: Int64) {
        reported = (streamID, endFrame)
    }

    func hasBuffered(stream streamID: UInt64, through frame: Int64) -> Bool {
        guard let reported else { return false }
        return reported.streamID == streamID && reported.endFrame >= frame
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

            // Fails only if the feeder overwrote these frames mid-copy; the next iteration
            // then finds `nextFrame` lapped and restarts.
            let copied = scratch.withUnsafeMutableBufferPointer {
                buffer.readInterleavedPCM16(nextFrame, Int32(count), $0.baseAddress!)
            }
            guard copied else { continue }

            let payload: Data
            if codec == .alac, let encoder {
                guard let packet = scratch.withUnsafeBufferPointer({ encoder.encode(interleaved: $0.baseAddress!, count: count) }) else {
                    AudioStreamServer.logger.error("ALAC encoding failed; skipping \(count) frames")
                    nextFrame += Int64(count)
                    continue
                }
                payload = packet
            } else {
                payload = scratch.withUnsafeBytes { Data($0.prefix(count * 4)) }
            }
            send(.audio(streamID: stream.id, startFrame: nextFrame, frameCount: count, payload: payload))
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
}
