//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation

/// Datagrams of the clock sync channel; the wire format is defined (and documented) by
/// `asyncmusic::ClockSyncMessage` in the core.
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

    func encoded() -> Data {
        let message: asyncmusic.ClockSyncMessage = switch self {
        case let .ping(sequence, t0):
            .makePing(sequence, t0)
        case let .pong(sequence, session, t0, t1, t2):
            .makePong(sequence, session, t0, t1, t2)
        case let .playback(revision, session, state):
            .makePlayback(revision, session, state)
        case let .audioPort(session, port):
            .makeAudioPort(session, port)
        }
        return Data(message.encode())
    }

    init?(_ data: Data) {
        var message = asyncmusic.ClockSyncMessage()
        let decoded = data.withUnsafeBytes { bytes in
            asyncmusic.ClockSyncMessage.decode(bytes.bindMemory(to: UInt8.self).baseAddress, bytes.count, &message)
        }
        guard decoded else { return nil }
        switch message.type {
        case .ping:
            self = .ping(sequence: message.sequence, t0: message.t0)
        case .pong:
            self = .pong(sequence: message.sequence, session: message.session, t0: message.t0, t1: message.t1, t2: message.t2)
        case .playback:
            self = .playback(revision: message.sequence, session: message.session, state: message.playback)
        case .audioPort:
            self = .audioPort(session: message.session, port: message.port)
        @unknown default:
            return nil
        }
    }
}
