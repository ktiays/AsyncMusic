//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import Network

/// Messages on the leader → follower audio stream (TCP). Each is sent as a frame:
///
///     length(4, little-endian, of what follows) type(1) payload
///
///     streamStart: streamID(8) source(8) sampleRate(8, Float64 bits) startFrame(8)
///     audio:       streamID(8) startFrame(8) frameCount(4) samples(frameCount × 2 × Int16, interleaved)
///
/// Frame numbers are in the source file's own sample rate, the same numbering the renderer
/// uses, so a follower can place every chunk without knowing anything about the leader's
/// playback.
nonisolated enum AudioStreamMessage {
    /// Starts a new stream. Audio for it follows contiguously from `startFrame`.
    case streamStart(streamID: UInt64, source: UInt64, sampleRate: Double, startFrame: Int64)
    /// Stereo samples as interleaved little-endian Int16.
    case audio(streamID: UInt64, startFrame: Int64, frameCount: Int, samples: Data)

    static let headerLength = 4
    /// Upper bound on a frame's length, to reject garbage before allocating for it.
    static let maximumBodyLength = 1 << 20

    private static let streamStartType: UInt8 = 1
    private static let audioType: UInt8 = 2

    /// The complete frame, header included.
    func framed() -> Data {
        var body = Data()
        switch self {
        case let .streamStart(streamID, source, sampleRate, startFrame):
            body.reserveCapacity(33)
            body.appendLittleEndian(Self.streamStartType)
            body.appendLittleEndian(streamID)
            body.appendLittleEndian(source)
            body.appendLittleEndian(sampleRate.bitPattern)
            body.appendLittleEndian(startFrame)
        case let .audio(streamID, startFrame, frameCount, samples):
            body.reserveCapacity(21 + samples.count)
            body.appendLittleEndian(Self.audioType)
            body.appendLittleEndian(streamID)
            body.appendLittleEndian(startFrame)
            body.appendLittleEndian(UInt32(frameCount))
            body.append(samples)
        }
        var frame = Data(capacity: Self.headerLength + body.count)
        frame.appendLittleEndian(UInt32(body.count))
        frame.append(body)
        return frame
    }

    /// Decodes the body length from a 4-byte header.
    static func bodyLength(header: Data) -> Int? {
        guard header.count == headerLength else { return nil }
        let length = Int(header.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(as: UInt32.self)) })
        return length > 0 && length <= maximumBodyLength ? length : nil
    }

    /// Decodes a frame body (everything after the header).
    init?(body: Data) {
        let body = Data(body)  // Rebase to index 0.
        guard let type = body.first else { return nil }
        func read<T: FixedWidthInteger>(_: T.Type, at offset: Int) -> T? {
            guard offset + MemoryLayout<T>.size <= body.count else { return nil }
            return body.withUnsafeBytes { T(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: T.self)) }
        }
        switch type {
        case Self.streamStartType:
            guard let streamID = read(UInt64.self, at: 1),
                  let source = read(UInt64.self, at: 9),
                  let rateBits = read(UInt64.self, at: 17),
                  let startFrame = read(Int64.self, at: 25)
            else { return nil }
            self = .streamStart(streamID: streamID, source: source, sampleRate: Double(bitPattern: rateBits), startFrame: startFrame)
        case Self.audioType:
            guard let streamID = read(UInt64.self, at: 1),
                  let startFrame = read(Int64.self, at: 9),
                  let frameCount = read(UInt32.self, at: 17),
                  body.count == 21 + Int(frameCount) * 4
            else { return nil }
            self = .audio(streamID: streamID, startFrame: startFrame, frameCount: Int(frameCount), samples: body.subdata(in: 21 ..< body.count))
        default:
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
    nonisolated mutating func appendLittleEndian<T: FixedWidthInteger>(_ value: T) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}
