//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network

typealias AudioStreamCodec = asyncmusic.AudioStreamCodec

/// Messages on the audio stream connection (TCP); the framing is defined (and documented) by
/// `asyncmusic::AudioStreamMessage` in the core.
nonisolated enum AudioStreamMessage {
    /// Starts a new stream. Audio for it follows contiguously from `startFrame`.
    case streamStart(streamID: UInt64, source: UInt64, sampleRate: Double, startFrame: Int64, codec: AudioStreamCodec, cookie: Data)
    /// `frameCount` stereo frames starting at `startFrame`, encoded with the stream's codec.
    case audio(streamID: UInt64, startFrame: Int64, frameCount: Int, payload: Data)
    /// How far the follower's buffer for `streamID` reaches (exclusive), so the leader knows
    /// when everyone has enough to start.
    case bufferReport(streamID: UInt64, endFrame: Int64)

    static let headerLength = Int(asyncmusic.kAudioStreamHeaderLength)

    /// The complete frame, header included.
    func framed() -> Data {
        let message: asyncmusic.AudioStreamMessage = switch self {
        case let .streamStart(streamID, source, sampleRate, startFrame, codec, cookie):
            .makeStreamStart(streamID, source, sampleRate, startFrame, codec, cookie.bytes)
        case let .audio(streamID, startFrame, frameCount, payload):
            .makeAudio(streamID, startFrame, Int32(frameCount), payload.bytes)
        case let .bufferReport(streamID, endFrame):
            .makeBufferReport(streamID, endFrame)
        }
        return Data(message.framed())
    }

    /// Decodes the body length from a 4-byte header.
    static func bodyLength(header: Data) -> Int? {
        let length = header.withUnsafeBytes { bytes in
            asyncmusic.AudioStreamMessage.bodyLength(bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count)
        }
        return length > 0 ? Int(length) : nil
    }

    /// Decodes a frame body (everything after the header).
    init?(body: Data) {
        var message = asyncmusic.AudioStreamMessage()
        let decoded = body.withUnsafeBytes { bytes in
            asyncmusic.AudioStreamMessage.decodeBody(bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count, &message)
        }
        guard decoded else { return nil }
        switch message.type {
        case .streamStart:
            self = .streamStart(streamID: message.streamID, source: message.source, sampleRate: message.sampleRate, startFrame: message.frame, codec: message.codec, cookie: Data(message.cookie))
        case .audio:
            self = .audio(streamID: message.streamID, startFrame: message.frame, frameCount: Int(message.frameCount), payload: Data(message.payload))
        case .bufferReport:
            self = .bufferReport(streamID: message.streamID, endFrame: message.frame)
        @unknown default:
            return nil
        }
    }
}

nonisolated enum AudioStreamNetwork {
    static func parameters() -> NWParameters {
        let tcp = NWProtocolTCP.Options()
        // Stream starts are small and latency-sensitive; audio chunks are large anyway.
        tcp.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: tcp)
        // Same reasoning as the clock sync parameters: AWDL adds latency spikes.
        parameters.includePeerToPeer = false
        return parameters
    }
}

private extension Data {
    nonisolated var bytes: asyncmusic.Bytes {
        withUnsafeBytes { asyncmusic.makeBytes($0.bindMemory(to: UInt8.self).baseAddress, $0.count) }
    }
}

nonisolated extension NWConnection {
    /// Reads framed `AudioStreamMessage`s until the connection ends or sends garbage, calling
    /// `onMessage` for each (on the connection's queue). `onEnd` gets a reason once.
    func receiveAudioStreamMessages(
        onMessage: @escaping @Sendable (AudioStreamMessage) -> Void,
        onEnd: @escaping @Sendable (String) -> Void
    ) {
        receive(minimumIncompleteLength: AudioStreamMessage.headerLength, maximumLength: AudioStreamMessage.headerLength) { [weak self] content, _, isComplete, error in
            guard let self else { return }
            guard let content, let length = AudioStreamMessage.bodyLength(header: content) else {
                onEnd(error?.localizedDescription ?? (isComplete ? "Connection closed" : "Malformed stream"))
                return
            }
            receive(minimumIncompleteLength: length, maximumLength: length) { [weak self] content, _, isComplete, error in
                guard let self else { return }
                guard let content, content.count == length else {
                    onEnd(error?.localizedDescription ?? (isComplete ? "Connection closed" : "Truncated message"))
                    return
                }
                if let message = AudioStreamMessage(body: content) {
                    onMessage(message)
                }
                receiveAudioStreamMessages(onMessage: onMessage, onEnd: onEnd)
            }
        }
    }
}
