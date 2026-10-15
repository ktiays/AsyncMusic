//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import Network

/// Follower side of audio streaming: receives the leader's stream over TCP into a
/// `StreamBuffer` that the player renders from exactly like a local file.
///
/// The client's queue is the buffer's only writer. Each stream start creates a fresh buffer
/// (handed to the player through `onStream`), since streams are append-only and a new one
/// usually starts somewhere else in the file.
nonisolated final class AudioStreamClient: @unchecked Sendable {
    enum State: Sendable, Equatable {
        case connecting
        case connected
        case reconnecting(String)
        case stopped
    }

    /// Seconds of audio a buffer holds: the leader's read-ahead plus room behind playback.
    let bufferDuration = 12.0

    var onStateChange: (@Sendable (State) -> Void)?
    /// A new stream began; play `source` from this buffer from now on.
    var onStream: (@Sendable (_ source: UInt64, _ buffer: StreamBuffer) -> Void)?
    var onLog: (@Sendable (String) -> Void)?

    private let endpoint: NWEndpoint
    private let queue = DispatchQueue(label: "me.ktiays.AsyncMusic.audio-stream.client", qos: .userInitiated)
    private var connection: NWConnection?
    private var running = false
    private var streamID: UInt64?
    private var buffer: StreamBuffer?
    private var left: [Float] = []
    private var right: [Float] = []

    init(endpoint: NWEndpoint) {
        self.endpoint = endpoint
    }

    func start() {
        queue.async { [self] in
            guard !running else { return }
            running = true
            onStateChange?(.connecting)
            connect()
        }
    }

    func stop() {
        queue.async { [self] in
            running = false
            connection?.cancel()
            connection = nil
            onStateChange?(.stopped)
        }
    }

    private func connect() {
        guard running else { return }
        let connection = NWConnection(to: endpoint, using: AudioStreamNetwork.parameters())
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard let self, let connection, connection === self.connection else { return }
            switch state {
            case .ready:
                onStateChange?(.connected)
                receiveHeader(on: connection)
            case let .waiting(error):
                onStateChange?(.reconnecting("Waiting: \(error.localizedDescription)"))
            case let .failed(error):
                reconnect(reason: error.localizedDescription)
            default:
                break
            }
        }
        self.connection = connection
        connection.start(queue: queue)
    }

    private func reconnect(reason: String) {
        guard running else { return }
        onStateChange?(.reconnecting(reason))
        connection?.cancel()
        connection = nil
        // The leader restarts the stream for every new connection anyway.
        streamID = nil
        queue.asyncAfter(deadline: .now() + .seconds(1)) { [self] in
            if connection == nil {
                connect()
            }
        }
    }

    private func receiveHeader(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: AudioStreamMessage.headerLength, maximumLength: AudioStreamMessage.headerLength) { [weak self, weak connection] content, _, isComplete, error in
            guard let self, let connection, connection === self.connection else { return }
            guard let content, let length = AudioStreamMessage.bodyLength(header: content) else {
                reconnect(reason: error?.localizedDescription ?? (isComplete ? "Leader closed the stream" : "Malformed stream"))
                return
            }
            receiveBody(length: length, on: connection)
        }
    }

    private func receiveBody(length: Int, on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: length, maximumLength: length) { [weak self, weak connection] content, _, isComplete, error in
            guard let self, let connection, connection === self.connection else { return }
            guard let content, content.count == length else {
                reconnect(reason: error?.localizedDescription ?? (isComplete ? "Leader closed the stream" : "Truncated message"))
                return
            }
            if let message = AudioStreamMessage(body: content) {
                handle(message)
            }
            receiveHeader(on: connection)
        }
    }

    private func handle(_ message: AudioStreamMessage) {
        switch message {
        case let .streamStart(streamID, source, sampleRate, startFrame):
            guard sampleRate > 0 else { return }
            let buffer = StreamBuffer(sampleRate: sampleRate, capacity: Int(bufferDuration * sampleRate), startFrame: startFrame)
            self.streamID = streamID
            self.buffer = buffer
            onLog?(String(format: "Stream %016llx: %.0f Hz from %.2fs", streamID, sampleRate, Double(startFrame) / sampleRate))
            onStream?(source, buffer)

        case let .audio(streamID, startFrame, frameCount, samples):
            // Chunks still in flight from a previous stream are expected after a switch.
            guard streamID == self.streamID, let buffer else { return }
            guard startFrame == buffer.endFrame else {
                onLog?("Stream discontinuity at frame \(startFrame), expected \(buffer.endFrame)")
                return
            }
            if left.count < frameCount {
                left = [Float](repeating: 0, count: frameCount)
                right = [Float](repeating: 0, count: frameCount)
            }
            samples.withUnsafeBytes { raw in
                for frame in 0 ..< frameCount {
                    let l = raw.loadUnaligned(fromByteOffset: frame * 4, as: Int16.self)
                    let r = raw.loadUnaligned(fromByteOffset: frame * 4 + 2, as: Int16.self)
                    left[frame] = Float(Int16(littleEndian: l)) / 32768
                    right[frame] = Float(Int16(littleEndian: r)) / 32768
                }
            }
            left.withUnsafeBufferPointer { l in
                right.withUnsafeBufferPointer { r in
                    buffer.append(left: l.baseAddress!, right: r.baseAddress!, count: frameCount)
                }
            }
        }
    }
}
