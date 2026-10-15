//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AVFoundation
import CryptoKit

/// An audio file decoded fully into memory at its own sample rate.
///
/// The renderer resamples on the fly, so the same track works whatever the output device's
/// sample rate is, and survives route changes without re-decoding.
nonisolated final class PCMTrack: @unchecked Sendable {
    /// Derived from the file's bytes, so the same file has the same ID on every device.
    let id: UInt64
    let name: String
    let duration: Double

    private let buffer: AVAudioPCMBuffer

    var samples: PCMSamples {
        let channels = buffer.floatChannelData!
        return PCMSamples(
            left: UnsafePointer(channels[0]),
            right: UnsafePointer(channels[buffer.format.channelCount > 1 ? 1 : 0]),
            frameCount: Int(buffer.frameLength),
            sampleRate: buffer.format.sampleRate
        )
    }

    private init(id: UInt64, name: String, buffer: AVAudioPCMBuffer) {
        self.id = id
        self.name = name
        self.buffer = buffer
        self.duration = Double(buffer.frameLength) / buffer.format.sampleRate
    }

    /// Reads and decodes the whole file. Slow; call off the main thread.
    static func load(contentsOf url: URL) throws -> PCMTrack {
        let accessing = url.startAccessingSecurityScopedResource()
        defer {
            if accessing {
                url.stopAccessingSecurityScopedResource()
            }
        }

        let digest = SHA256.hash(data: try Data(contentsOf: url, options: .mappedIfSafe))
        let hash = digest.withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) }
        // Keep clear of the reserved IDs (none, metronome).
        let id = max(hash, PlaybackSource.metronome + 1)

        let file = try AVAudioFile(forReading: url)
        guard let buffer = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: AVAudioFrameCount(file.length)) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        try file.read(into: buffer)
        return PCMTrack(id: id, name: url.deletingPathExtension().lastPathComponent, buffer: buffer)
    }
}
