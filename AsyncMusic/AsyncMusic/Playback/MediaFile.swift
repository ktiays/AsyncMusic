//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AVFoundation
import CryptoKit

/// A local audio file that can be played. Only its header is read up front; the audio is
/// decoded a few seconds ahead of playback by `FileStreamFeeder`.
nonisolated final class MediaFile: @unchecked Sendable {
    /// Derived from the file's bytes, so the same file has the same ID on every device.
    let id: UInt64
    let name: String
    let url: URL
    let sampleRate: Double
    let frameCount: Int64

    var duration: Double { Double(frameCount) / sampleRate }

    private let isAccessingSecurityScope: Bool

    private init(id: UInt64, url: URL, sampleRate: Double, frameCount: Int64, isAccessingSecurityScope: Bool) {
        self.id = id
        self.name = url.deletingPathExtension().lastPathComponent
        self.url = url
        self.sampleRate = sampleRate
        self.frameCount = frameCount
        self.isAccessingSecurityScope = isAccessingSecurityScope
    }

    deinit {
        if isAccessingSecurityScope {
            url.stopAccessingSecurityScopedResource()
        }
    }

    /// Hashes the file and reads its format. Reads the whole file once; call off the main thread.
    static func load(contentsOf url: URL) throws -> MediaFile {
        // Keep access for as long as this object lives: the feeder reopens the file per stream.
        let accessing = url.startAccessingSecurityScopedResource()
        do {
            let digest = SHA256.hash(data: try Data(contentsOf: url, options: .mappedIfSafe))
            let hash = digest.withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) }
            let file = try AVAudioFile(forReading: url)
            return MediaFile(
                // Keep clear of the reserved IDs (none, metronome).
                id: max(hash, PlaybackSource.metronome + 1),
                url: url,
                sampleRate: file.processingFormat.sampleRate,
                frameCount: file.length,
                isAccessingSecurityScope: accessing
            )
        } catch {
            if accessing {
                url.stopAccessingSecurityScopedResource()
            }
            throw error
        }
    }
}
