//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import AVFoundation
import Observation
import os
#if canImport(UIKit)
import UIKit
#endif

/// Owns the audio engine and keeps the `SyncedRenderer` fed with the clock map, the playback
/// state and this device's output latency. Files are played through a `StreamBuffer`: either
/// one a `FileStreamFeeder` keeps filled a few seconds ahead from a local file (the leader, or
/// a follower that has the same file), or one filled by the leader's audio stream.
///
/// While started, the engine runs continuously (rendering silence when nothing plays). On iOS,
/// together with the `audio` background mode, that is what keeps the app — and with it the
/// clock sync connection — alive in the background. `stop()` releases it so an idle app can be
/// suspended normally.
///
/// Keeping a silent engine running indefinitely is not an acceptable use of background audio,
/// so after `idleReleaseDelay` in the background without playback the engine is released too.
/// iOS then suspends the app and the device drops out of the group until it is reopened.
@Observable
final class SyncedPlayer {
    private(set) var isRunning = false
    private(set) var engineError: String?
    private(set) var outputSampleRate: Double = 0
    /// Reported time from the render timestamp to the sound leaving the device, in seconds.
    private(set) var reportedLatency: Double = 0
    private(set) var ioBufferDuration: Double = 0
    private(set) var playback = PlaybackState.stopped()
    private(set) var files: [UInt64: MediaFile] = [:]
    /// The speaker channel this device plays (or will, once the scheduled switch is heard).
    private(set) var channel = SpeakerChannel.full

    /// Extra delay on top of the reported latency, in milliseconds. Increase it when this
    /// device sounds late compared with the others (common with Bluetooth, whose reported
    /// latency is often off by tens of milliseconds).
    var manualOffset: Double = UserDefaults.standard.double(forKey: SyncedPlayer.manualOffsetKey) {
        didSet {
            UserDefaults.standard.set(manualOffset, forKey: Self.manualOffsetKey)
            pushPresentationDelay()
        }
    }

    /// The source of the stream most recently received from the leader.
    private(set) var remoteStreamSource: UInt64?

    /// True when a file is playing that this device has neither locally nor from the leader.
    var isWaitingForAudio: Bool {
        playback.isPlaying() && !PlaybackSource.isGenerated(playback.source)
            && files[playback.source] == nil && remoteStreamSource != playback.source
    }

    /// Called whenever playback switches to (or away from) a locally decoded file stream, so
    /// a leader can forward it to followers.
    @ObservationIgnored var onOutgoingStream: ((OutgoingStream?) -> Void)?

    var renderStats: RenderStats { renderer.latestStats() }

    /// How long the app stays alive in the background with nothing playing.
    let idleReleaseDelay: Duration = .seconds(5 * 60)
    /// True after the engine was released for being idle in the background.
    private(set) var isReleasedForIdle = false

    @ObservationIgnored private var engine = AVAudioEngine()
    @ObservationIgnored private let renderer = SyncedRenderer.create()
    @ObservationIgnored private var sourceNode: AVAudioSourceNode?
    @ObservationIgnored private var observers: [NSObjectProtocol] = []
    @ObservationIgnored private var engineObserver: NSObjectProtocol?
    /// Whether the engine should be running; the system may stop it behind our back.
    @ObservationIgnored private var wantsRunning = false
    @ObservationIgnored private var playbackGeneration = 0
    @ObservationIgnored private var isInBackground = false
    @ObservationIgnored private var idleReleaseTask: Task<Void, Never>?
    /// Readable from the feeder's and stream senders' queues, which pace on the heard position.
    @ObservationIgnored private let position = PlaybackPosition.create()
    @ObservationIgnored private var activeFeeder: FileStreamFeeder?
    /// Buffers recently swapped out, oldest first. The renderer holds them unretained and may
    /// keep using one until its next successful try-lock, so they are released after a delay.
    @ObservationIgnored private var retiredBuffers: [StreamBuffer] = []
    @ObservationIgnored private var remoteBuffer: StreamBuffer?

    /// How far ahead of playback files are decoded.
    private let streamAheadDuration = 8.0

    private static let manualOffsetKey = "SyncedPlayer.manualOffset"
    private static let logger = Logger(subsystem: "me.ktiays.AsyncMusic", category: "Playback")

    func start() {
        guard !wantsRunning else { return }
        wantsRunning = true
        observeSystemChanges()
        observeEngine()
        startEngine()
    }

    func stop() {
        guard wantsRunning else { return }
        wantsRunning = false
        isReleasedForIdle = false
        idleReleaseTask?.cancel()
        releaseEngine()
    }

    private func releaseEngine() {
        engine.stop()
        isRunning = false
        #if !os(macOS)
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        #endif
    }

    func update(clockMap: ClockMap?) {
        renderer.setClockMap(clockMap)
        let wasUnknown = !position.hasClockMap()
        position.setClockMap(clockMap)
        // A file stream can only start once we know where playback is.
        if wasUnknown, clockMap != nil, activeFeeder == nil, files[playback.source] != nil {
            applySource()
        }
    }

    func update(playback: PlaybackState) {
        // Re-applying the same state would needlessly reposition the read head.
        guard playback != self.playback else { return }
        let previous = self.playback
        self.playback = playback
        position.setPlayback(playback)
        if let feeder = activeFeeder, canKeepStream(from: previous, to: playback) {
            // Only the schedule changed (e.g. the leader moved the start once everyone had
            // buffered): keep decoding into, and streaming from, the same buffer.
            installSource(.stream(feeder.buffer), reposition: true)
        } else {
            applySource()
        }
        updateIdleRelease()
    }

    func add(_ file: MediaFile) {
        files[file.id] = file
        if playback.source == file.id {
            applySource()
        }
    }

    /// Plays only `channel` of the source (on all of this device's outputs) from leader time
    /// `leaderTime` on, as heard; pass `.min` to switch right away.
    func setChannel(_ channel: SpeakerChannel, at leaderTime: Int64 = .min) {
        self.channel = channel
        renderer.setChannelMix(.forChannel(channel), leaderTime)
    }

    /// Plays `source` from `buffer`, which the leader's stream fills, unless the file is
    /// available locally. Replaces any previous remote stream.
    func setRemoteStream(_ buffer: StreamBuffer, source: UInt64) {
        if let remoteBuffer {
            retire(remoteBuffer)
        }
        remoteBuffer = buffer
        remoteStreamSource = source
        if playback.source == source, files[source] == nil {
            // Playback position is derived from time, not from the buffer, so swapping the
            // buffer under a playing renderer needs no repositioning.
            applySource(reposition: false)
        }
    }

    // MARK: - Engine

    private func configureGraph() throws {
        if let sourceNode {
            engine.detach(sourceNode)
        }
        let sampleRate = engine.outputNode.outputFormat(forBus: 0).sampleRate
        guard sampleRate > 0, let format = AVAudioFormat(standardFormatWithSampleRate: sampleRate, channels: 2) else {
            throw CocoaError(.featureUnsupported, userInfo: [NSLocalizedDescriptionKey: "No audio output device"])
        }
        let node = Self.makeSourceNode(renderer: renderer, format: format)
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
        sourceNode = node
        outputSampleRate = sampleRate
    }

    /// Built outside the main actor so the render block is not main-actor isolated: it runs on
    /// the real-time audio thread.
    private nonisolated static func makeSourceNode(renderer: SyncedRenderer, format: AVAudioFormat) -> AVAudioSourceNode {
        let sampleRate = format.sampleRate
        return AVAudioSourceNode(format: format) { _, timestamp, frameCount, audioBufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard buffers.count >= 2,
                  let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let right = buffers[1].mData?.assumingMemoryBound(to: Float.self)
            else { return noErr }

            let stamp = timestamp.pointee
            let hostTime = stamp.mFlags.contains(.hostTimeValid)
                ? HostClock.nanoseconds(fromHostTicks: stamp.mHostTime)
                : nil
            renderer.render(hostTime != nil, hostTime ?? 0, Int32(frameCount), sampleRate, left, right)
            return noErr
        }
    }

    private func refreshLatency() {
        #if os(macOS)
        // On macOS the output render timestamp is already the time the buffer reaches the
        // device, so only the device's own presentation latency remains.
        reportedLatency = engine.outputNode.presentationLatency
        ioBufferDuration = 0
        #else
        let session = AVAudioSession.sharedInstance()
        reportedLatency = session.outputLatency
        ioBufferDuration = session.ioBufferDuration
        #endif
        pushPresentationDelay()
    }

    private func pushPresentationDelay() {
        let delay = Int64(((reportedLatency + manualOffset / 1000) * 1e9).rounded())
        renderer.setPresentationDelay(delay)
    }

    private func canKeepStream(from previous: PlaybackState, to playback: PlaybackState) -> Bool {
        guard let feeder = activeFeeder, previous.source == playback.source, playback.isPlaying(),
              let heard = position.heardMediaTime()
        else { return false }
        let buffer = feeder.buffer
        let frame = Int64((Double(max(playback.startMediaTime, heard, 0)) * buffer.sampleRate() / 1e9).rounded(.down))
        // The decoder only moves forward from where the buffer ends, so the new position must
        // lie within what the buffer holds or will soon hold.
        let range = buffer.validRange()
        return frame >= range.lowerBound && frame <= range.upperBound + Int64(buffer.sampleRate())
    }

    private func applySource(reposition: Bool = true) {
        let hadOutgoingStream = activeFeeder != nil
        stopStream()
        let source: RenderSource
        if !playback.isPlaying() {
            source = .silence
        } else if playback.source == PlaybackSource.metronome {
            source = .metronome
        } else if playback.source == PlaybackSource.channelTest {
            source = .channelTest
        } else if let file = files[playback.source] {
            source = startStream(for: file).map { .stream($0) } ?? .silence
        } else if let remoteBuffer, remoteStreamSource == playback.source {
            source = .stream(remoteBuffer)
        } else {
            source = .silence
        }
        if hadOutgoingStream, activeFeeder == nil {
            onOutgoingStream?(nil)
        }
        installSource(source, reposition: reposition)
    }

    private func installSource(_ source: RenderSource, reposition: Bool) {
        if reposition {
            playbackGeneration += 1
        }
        switch source {
        case .silence:
            renderer.setPlayback(playback, Int32(playbackGeneration), .silence, nil)
        case .metronome:
            renderer.setPlayback(playback, Int32(playbackGeneration), .metronome, nil)
        case .channelTest:
            renderer.setPlayback(playback, Int32(playbackGeneration), .channelTest, nil)
        case let .stream(buffer):
            renderer.setPlayback(playback, Int32(playbackGeneration), .stream, buffer)
        }
    }

    private enum RenderSource {
        case silence
        case metronome
        case channelTest
        case stream(StreamBuffer)
    }

    /// Starts decoding `file` from just before the position being heard now (or from the
    /// scheduled start, if that is still ahead). Returns nil until the clock map is known.
    private func startStream(for file: MediaFile) -> StreamBuffer? {
        guard let heardNow = position.heardMediaTime() else { return nil }
        let rate = file.sampleRate
        // A little slack before "now" so a late join loses nothing to the time decoding takes.
        let startTime = max(playback.startMediaTime, heardNow - 100_000_000, 0)
        let startFrame = Int64((Double(startTime) * rate / 1e9).rounded(.down))

        // Room for the read-ahead plus a few seconds behind playback, which the writer may
        // overwrite only once they have been heard.
        let buffer = StreamBuffer.make(sampleRate: rate, capacity: Int((streamAheadDuration + 4) * rate), startFrame: startFrame)
        var configuration = FileStreamFeeder.Configuration()
        configuration.aheadDuration = streamAheadDuration
        let position = position
        let feeder = FileStreamFeeder(file: file, buffer: buffer, startFrame: startFrame, configuration: configuration) {
            position.heardMediaTime()
        }
        feeder.onStall = { [weak self, weak feeder] in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let self, let feeder, self.activeFeeder === feeder else { return }
                    self.applySource()
                }
            }
        }
        activeFeeder = feeder
        feeder.start()
        onOutgoingStream?(OutgoingStream(id: .random(in: .min ... .max), source: file.id, buffer: buffer, position: position))
        return buffer
    }

    private func stopStream() {
        guard let feeder = activeFeeder else { return }
        activeFeeder = nil
        feeder.stop()
        retire(feeder.buffer)
    }

    private func retire(_ buffer: StreamBuffer) {
        retiredBuffers.append(buffer)
        // Every buffer waits the same time, so the oldest one is always the one that is due.
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(2))
            self?.retiredBuffers.removeFirst()
        }
    }

    private func startEngine() {
        do {
            #if !os(macOS)
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default)
            // Shorter IO buffers do not improve sync accuracy (the renderer is sample accurate
            // either way), but they make the reported latency smaller and more stable.
            try session.setPreferredIOBufferDuration(0.005)
            try session.setActive(true)
            #endif
            try configureGraph()
            try engine.start()
            isRunning = true
            engineError = nil
        } catch {
            isRunning = false
            engineError = error.localizedDescription
            Self.logger.error("Engine failed to start: \(error, privacy: .public)")
        }
        refreshLatency()
    }

    private func restartEngine(reason: String) {
        guard wantsRunning, !isReleasedForIdle else { return }
        Self.logger.info("Restarting engine: \(reason, privacy: .public)")
        engine.stop()
        startEngine()
    }

    /// After a media services reset every audio object is invalid, so start from scratch.
    private func rebuildEngine() {
        guard wantsRunning, !isReleasedForIdle else { return }
        Self.logger.info("Media services were reset; rebuilding engine")
        engine.stop()
        engine = AVAudioEngine()
        sourceNode = nil
        observeEngine()
        startEngine()
    }

    private func observeEngine() {
        if let engineObserver {
            NotificationCenter.default.removeObserver(engineObserver)
        }
        // Sample rate or device changes stop the engine; rebuild for the new format.
        engineObserver = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.restartEngine(reason: "configuration changed") }
        }
    }

    private func observeSystemChanges() {
        #if !os(macOS)
        guard observers.isEmpty else { return }
        let center = NotificationCenter.default
        // Plugging in headphones or connecting Bluetooth changes the latency.
        observers.append(center.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.refreshLatency() }
        })
        // A call, Siri or another app's non-mixable audio stops the engine. Resume as soon as
        // the interruption ends: falling silent in the background would let iOS suspend us.
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] notification in
            let type = (notification.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt)
                .flatMap(AVAudioSession.InterruptionType.init(rawValue:))
            MainActor.assumeIsolated {
                switch type {
                case .began:
                    self?.isRunning = false
                case .ended:
                    self?.restartEngine(reason: "interruption ended")
                default:
                    break
                }
            }
        })
        observers.append(center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.rebuildEngine() }
        })
        observers.append(center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.isInBackground = true
                self?.updateIdleRelease()
            }
        })
        // Coming back restarts an engine released for idleness, and is also the last resort if
        // anything above left it stopped (e.g. an interruption whose end was never delivered).
        observers.append(center.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.isInBackground = false
                self.updateIdleRelease()
                if self.isReleasedForIdle {
                    self.isReleasedForIdle = false
                    self.restartEngine(reason: "back in the foreground after idle release")
                } else if !self.engine.isRunning {
                    self.restartEngine(reason: "app became active")
                }
            }
        })
        #endif
    }

    /// Arms the idle release while in the background with nothing playing, disarms it otherwise.
    private func updateIdleRelease() {
        idleReleaseTask?.cancel()
        idleReleaseTask = nil
        guard wantsRunning, isInBackground, !playback.isPlaying(), !isReleasedForIdle else { return }
        idleReleaseTask = Task { [weak self, idleReleaseDelay] in
            try? await Task.sleep(for: idleReleaseDelay)
            guard !Task.isCancelled, let self else { return }
            Self.logger.info("Idle in the background; releasing audio so the app can be suspended")
            self.isReleasedForIdle = true
            self.releaseEngine()
        }
    }
}
