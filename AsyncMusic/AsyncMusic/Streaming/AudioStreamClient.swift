//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network

/// Follower side of audio streaming: receives the leader's stream over TCP into a
/// `StreamBuffer` that the player renders from exactly like a local file.
///
/// The client's queue is the buffer's only writer. Each stream start creates a fresh buffer
/// (handed to the player through `onStream`), since streams are append-only and a new one
/// usually starts somewhere else in the file. After every chunk the client reports how far
/// its buffer reaches, which the leader uses to decide when everyone is ready to start.
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
    private var codec = AudioStreamCodec.pcm16
    private var decoder: ALACDecoder?
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
                receive(on: connection)
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

    private func receive(on connection: NWConnection) {
        connection.receiveAudioStreamMessages { [weak self, weak connection] message in
            guard let self, let connection, connection === self.connection else { return }
            handle(message, on: connection)
        } onEnd: { [weak self, weak connection] reason in
            guard let self, let connection, connection === self.connection else { return }
            reconnect(reason: reason)
        }
    }

    private func handle(_ message: AudioStreamMessage, on connection: NWConnection) {
        switch message {
        case let .streamStart(streamID, source, sampleRate, startFrame, codec, cookie):
            guard sampleRate > 0 else { return }
            if codec == .alac {
                guard let decoder = ALACDecoder(sampleRate: sampleRate, magicCookie: cookie) else {
                    onLog?("Cannot decode stream \(String(format: "%016llx", streamID)): ALAC decoder unavailable")
                    self.streamID = nil
                    return
                }
                self.decoder = decoder
            } else {
                decoder = nil
            }
            self.codec = codec
            let buffer = StreamBuffer.make(sampleRate: sampleRate, capacity: Int(bufferDuration * sampleRate), startFrame: startFrame)
            self.streamID = streamID
            self.buffer = buffer
            onLog?(String(format: "Stream %016llx: %.0f Hz %@ from %.2fs", streamID, sampleRate, codec == .alac ? "ALAC" : "PCM", Double(startFrame) / sampleRate))
            onStream?(source, buffer)

        case .bufferReport:
            break

        case let .audio(streamID, startFrame, frameCount, payload):
            // Chunks still in flight from a previous stream are expected after a switch.
            guard streamID == self.streamID, let buffer else { return }
            guard startFrame == buffer.endFrame() else {
                onLog?("Stream discontinuity at frame \(startFrame), expected \(buffer.endFrame())")
                return
            }
            switch codec {
            case .alac:
                guard let decoder, decoder.decode(payload, frameCount: frameCount) == frameCount else {
                    onLog?("Could not decode chunk at frame \(startFrame)")
                    return
                }
                buffer.append(left: decoder.left, right: decoder.right, count: frameCount)
            case .pcm16:
                if left.count < frameCount {
                    left = [Float](repeating: 0, count: frameCount)
                    right = [Float](repeating: 0, count: frameCount)
                }
                let decoded = payload.withUnsafeBytes { raw in
                    left.withUnsafeMutableBufferPointer { l in
                        right.withUnsafeMutableBufferPointer { r in
                            asyncmusic.decodePCM16(raw.bindMemory(to: UInt8.self).baseAddress!, raw.count, Int32(frameCount), l.baseAddress!, r.baseAddress!)
                        }
                    }
                }
                guard decoded else { return }
                left.withUnsafeBufferPointer { l in
                    right.withUnsafeBufferPointer { r in
                        buffer.append(left: l.baseAddress!, right: r.baseAddress!, count: frameCount)
                    }
                }
            }
            let report = AudioStreamMessage.bufferReport(streamID: streamID, endFrame: buffer.endFrame())
            connection.send(content: report.framed(), completion: .idempotent)
        }
    }
}
