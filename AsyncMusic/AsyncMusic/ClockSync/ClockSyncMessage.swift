//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation

/// Wire format of the clock sync probes. Fixed-size little-endian datagrams:
///
///     ping: magic(2) version(1) type(1) sequence(4) t0(8)
///     pong: magic(2) version(1) type(1) sequence(4) session(8) t0(8) t1(8) t2(8)
///     playback: magic(2) version(1) type(1) revision(4) session(8) source(8) startLeaderTime(8) startMediaTime(8)
///     audioPort: magic(2) version(1) type(1) unused(4) session(8) port(2)
nonisolated enum ClockSyncMessage: Sendable, Equatable {
    case ping(sequence: UInt32, t0: Int64)
    /// `session` identifies one run of the leader; a new value means its timeline changed.
    case pong(sequence: UInt32, session: UInt64, t0: Int64, t1: Int64, t2: Int64)
    /// Sent by the leader whenever playback changes and repeated periodically, since UDP may
    /// drop it. `revision` increases with every change within one leader session.
    case playback(revision: UInt32, session: UInt64, state: PlaybackState)
    /// The TCP port of the leader's audio stream, on the same host as the clock service.
    /// Sent with the playback state, so followers need no second Bonjour lookup.
    case audioPort(session: UInt64, port: UInt16)

    private static let magic: UInt16 = 0x4153  // "AS"
    private static let version: UInt8 = 1
    private static let pingType: UInt8 = 1
    private static let pongType: UInt8 = 2
    private static let playbackType: UInt8 = 3
    private static let audioPortType: UInt8 = 4

    func encoded() -> Data {
        var data = Data(capacity: 48)
        data.append(littleEndian: Self.magic)
        data.append(littleEndian: Self.version)
        switch self {
        case let .ping(sequence, t0):
            data.append(littleEndian: Self.pingType)
            data.append(littleEndian: sequence)
            data.append(littleEndian: t0)
        case let .pong(sequence, session, t0, t1, t2):
            data.append(littleEndian: Self.pongType)
            data.append(littleEndian: sequence)
            data.append(littleEndian: session)
            data.append(littleEndian: t0)
            data.append(littleEndian: t1)
            data.append(littleEndian: t2)
        case let .playback(revision, session, state):
            data.append(littleEndian: Self.playbackType)
            data.append(littleEndian: revision)
            data.append(littleEndian: session)
            data.append(littleEndian: state.source)
            data.append(littleEndian: state.startLeaderTime)
            data.append(littleEndian: state.startMediaTime)
        case let .audioPort(session, port):
            data.append(littleEndian: Self.audioPortType)
            data.append(littleEndian: UInt32(0))
            data.append(littleEndian: session)
            data.append(littleEndian: port)
        }
        return data
    }

    init?(_ data: Data) {
        var reader = Reader(data: data)
        guard reader.read(UInt16.self) == Self.magic,
              reader.read(UInt8.self) == Self.version,
              let type = reader.read(UInt8.self),
              let sequence = reader.read(UInt32.self)
        else { return nil }

        switch type {
        case Self.pingType:
            guard let t0 = reader.read(Int64.self) else { return nil }
            self = .ping(sequence: sequence, t0: t0)
        case Self.pongType:
            guard let session = reader.read(UInt64.self),
                  let t0 = reader.read(Int64.self),
                  let t1 = reader.read(Int64.self),
                  let t2 = reader.read(Int64.self)
            else { return nil }
            self = .pong(sequence: sequence, session: session, t0: t0, t1: t1, t2: t2)
        case Self.playbackType:
            guard let session = reader.read(UInt64.self),
                  let source = reader.read(UInt64.self),
                  let startLeaderTime = reader.read(Int64.self),
                  let startMediaTime = reader.read(Int64.self)
            else { return nil }
            let state = PlaybackState(source: source, startLeaderTime: startLeaderTime, startMediaTime: startMediaTime)
            self = .playback(revision: sequence, session: session, state: state)
        case Self.audioPortType:
            guard let session = reader.read(UInt64.self),
                  let port = reader.read(UInt16.self)
            else { return nil }
            self = .audioPort(session: session, port: port)
        default:
            return nil
        }
    }

    private struct Reader {
        let data: Data
        var position: Int

        init(data: Data) {
            self.data = data
            self.position = data.startIndex
        }

        mutating func read<T: FixedWidthInteger>(_: T.Type) -> T? {
            let size = MemoryLayout<T>.size
            guard position + size <= data.endIndex else { return nil }
            var value: T = 0
            withUnsafeMutableBytes(of: &value) { buffer in
                _ = data.copyBytes(to: buffer, from: position ..< position + size)
            }
            position += size
            return T(littleEndian: value)
        }
    }
}

private extension Data {
    nonisolated mutating func append<T: FixedWidthInteger>(littleEndian value: T) {
        Swift.withUnsafeBytes(of: value.littleEndian) { append(contentsOf: $0) }
    }
}
