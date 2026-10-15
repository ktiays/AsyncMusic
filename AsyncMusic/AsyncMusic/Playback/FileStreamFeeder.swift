//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import AVFoundation
import os

/// Decodes a `MediaFile` into a `StreamBuffer` in small chunks, staying `aheadDuration` ahead
/// of playback.
///
/// This is the local stand-in for a network stream: the buffer does not care where its frames
/// come from, and pacing works the same way, from the media time currently being heard.
nonisolated final class FileStreamFeeder: @unchecked Sendable {
    struct Configuration {
        /// How far ahead of the playing position to decode. Must stay well below the buffer's
        /// capacity, since frames older than `capacity` before the newest are overwritten.
        var aheadDuration = 8.0
        var chunkFrames: AVAudioFrameCount = 4096
        var interval: DispatchTimeInterval = .milliseconds(100)
        /// Falling this far behind the playing position counts as a stall.
        var stallThreshold = 0.5
    }

    let configuration: Configuration
    let buffer: StreamBuffer

    /// Called on the feeder's queue when decoding fell behind playback (e.g. the app was
    /// suspended). The buffer is append-only, so the owner should start a new stream.
    var onStall: (@Sendable () -> Void)?

    private let file: MediaFile
    private let startFrame: Int64
    /// The media time (nanoseconds) being heard now, or nil while unknown.
    private let currentMediaTime: @Sendable () -> Int64?
    private let queue = DispatchQueue(label: "me.ktiays.AsyncMusic.file-stream", qos: .userInitiated)
    private var audioFile: AVAudioFile?
    private var chunk: AVAudioPCMBuffer?
    private var timer: DispatchSourceTimer?
    private var finished = false
    private var stalled = false

    private static let logger = Logger(subsystem: "me.ktiays.AsyncMusic", category: "Playback")

    /// - Parameters:
    ///   - startFrame: First frame to decode; the buffer must have been created at this frame.
    ///   - currentMediaTime: Called on the feeder's queue to pace decoding.
    init(
        file: MediaFile,
        buffer: StreamBuffer,
        startFrame: Int64,
        configuration: Configuration = Configuration(),
        currentMediaTime: @escaping @Sendable () -> Int64?
    ) {
        self.file = file
        self.buffer = buffer
        self.startFrame = startFrame
        self.configuration = configuration
        self.currentMediaTime = currentMediaTime
    }

    func start() {
        queue.async { [self] in
            guard open() else { return }
            fill()
            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + configuration.interval, repeating: configuration.interval)
            timer.setEventHandler { [weak self] in
                self?.fill()
            }
            timer.resume()
            self.timer = timer
        }
    }

    func stop() {
        queue.async { [self] in
            timer?.cancel()
            timer = nil
            audioFile = nil
            finished = true
        }
    }

    /// Opens the file at `startFrame`. Runs on the feeder's queue (or synchronously in tests).
    func open() -> Bool {
        do {
            let audioFile = try AVAudioFile(forReading: file.url)
            audioFile.framePosition = min(max(startFrame, 0), audioFile.length)
            self.audioFile = audioFile
            chunk = AVAudioPCMBuffer(pcmFormat: audioFile.processingFormat, frameCapacity: configuration.chunkFrames)
            return chunk != nil
        } catch {
            Self.logger.error("Could not open \(self.file.name, privacy: .public): \(error, privacy: .public)")
            finished = true
            return false
        }
    }

    /// Decodes until the buffer reaches `aheadDuration` past the playing position.
    func fill() {
        guard !finished, let audioFile, let chunk else { return }
        let rate = buffer.sampleRate()
        let playingFrame = currentMediaTime().map { Int64((Double($0) * rate / 1e9).rounded(.down)) }
        // Until the position is known, just prebuffer from the start.
        let target = (playingFrame ?? startFrame) + Int64(configuration.aheadDuration * rate)

        if let playingFrame, playingFrame - buffer.endFrame() > Int64(configuration.stallThreshold * rate),
           buffer.endFrame() < file.frameCount, !stalled {
            stalled = true
            Self.logger.info("File stream fell behind playback; requesting a restart")
            onStall?()
            return
        }

        while buffer.endFrame() < target {
            chunk.frameLength = 0
            do {
                try audioFile.read(into: chunk, frameCount: configuration.chunkFrames)
            } catch {
                Self.logger.error("Decoding failed: \(error, privacy: .public)")
                finish()
                return
            }
            guard chunk.frameLength > 0, let channels = chunk.floatChannelData else {
                finish()
                return
            }
            let right = chunk.format.channelCount > 1 ? channels[1] : channels[0]
            buffer.append(left: channels[0], right: right, count: Int(chunk.frameLength))
        }
    }

    private func finish() {
        finished = true
        timer?.cancel()
        timer = nil
        audioFile = nil
    }
}
