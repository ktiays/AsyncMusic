//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation
import Network
import Observation
import UniformTypeIdentifiers
#if os(macOS)
import AppKit
import IOKit.ps
#else
import UIKit
#endif

typealias ShareDiscovery = asyncmusic.ShareDiscovery

/// The share model: whether this device listens for shares or shares its own audio, the shares
/// heard nearby and the alerts for new ones, and the group session doing the playing. On a Mac
/// it runs in AsyncMusic Server; iPhone, iPad and Vision Pro run it in the app.
///
/// Listening and sharing exclude each other, as `ShareDiscovery` does on the network. Listening
/// is the default, so alerts arrive without opening anything. Joining a share follows the
/// sharer's clock leader at the address it announced; sharing leads, and announces the leader's
/// port once it is listening.
@Observable
final class ShareCenter {
    let session = ClockSyncSession()

    private(set) var settings: ServiceSettings
    private(set) var mode = ShareMode.listen
    private(set) var nearby: [NearbyShare] = []
    /// New shares waiting for an answer, newest first.
    private(set) var alerts: [NearbyShare] = []
    /// Set when an alert went unanswered; cleared when the panel opens.
    private(set) var hasUnseenShares = false
    private(set) var joined: NearbyShare?

    private(set) var shareSource: ShareSource?
    private(set) var isSharing = false
    private(set) var discoveryError: String?
    #if os(macOS)
    let catalog = AudioApplicationCatalog()
    #endif

    /// Listening: this device keeps its place in the group but plays nothing (the system's pause).
    private(set) var isListeningPaused = false
    /// Sharing a file: where it was paused, in media nanoseconds; nil while it plays (or before).
    private(set) var pausedPosition: Int64?

    /// Called when a new alert arrives, for the window that shows it.
    @ObservationIgnored var onAlert: (() -> Void)?

    @ObservationIgnored private let discovery: ShareDiscovery
    @ObservationIgnored private var generation: UInt64 = 0
    @ObservationIgnored private var seenShares: Set<UInt64> = []
    @ObservationIgnored private var announcedInfo: asyncmusic.ShareInfo?
    // Observed: the state shows "Loading…" while it is set.
    private var playsWhenLoaded = false
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    #if os(macOS)
    @ObservationIgnored private var openPanel: NSOpenPanel?
    #endif

    private static let settingsKey = "ShareCenter.settings"

    init() {
        let defaults = UserDefaults.standard
        settings = defaults.data(forKey: Self.settingsKey).flatMap { try? JSONDecoder().decode(ServiceSettings.self, from: $0) } ?? ServiceSettings()
        discovery = ShareDiscovery.create(session.identity.id)
        discovery.listen()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                self?.poll()
                do { try await Task.sleep(for: .milliseconds(200)) } catch { return }
            }
        }
    }

    deinit {
        pollTask?.cancel()
    }

    var deviceName: String { settings.deviceName }

    var listeners: [ClockSyncLeader.Member] { session.members.filter(\.isActive) }

    func updateSettings(_ settings: ServiceSettings) {
        guard settings != self.settings else { return }
        self.settings = settings
        let defaults = UserDefaults.standard
        defaults.set(try? JSONEncoder().encode(settings), forKey: Self.settingsKey)
        defaults.set(settings.customDeviceName, forKey: DeviceIdentity.customNameKey)
    }

    func setMode(_ mode: ShareMode) {
        guard mode != self.mode else { return }
        if joined != nil { leave() }
        if isSharing { stopSharing() }
        self.mode = mode
        alerts = []
        switch mode {
        case .listen:
            discovery.listen()
        case .share:
            // A device sharing its own audio does not take part in others' shares.
            discovery.stop()
            nearby = []
            seenShares = []
            generation = 0
        }
    }

    // MARK: - Listening

    func join(_ id: UInt64) {
        guard let share = nearby.first(where: { $0.id == id }) ?? alerts.first(where: { $0.id == id }) else { return }
        join(share)
    }

    func join(_ share: NearbyShare) {
        if mode != .listen { setMode(.listen) }
        alerts.removeAll { $0.id == share.id }
        if joined?.id == share.id { return }
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(share.host), port: NWEndpoint.Port(rawValue: share.port) ?? .any)
        session.setRole(.follower)
        session.follow(ClockSyncSession.DiscoveredLeader(name: share.name, endpoint: endpoint))
        joined = share
    }

    func leave() {
        guard joined != nil else { return }
        session.setRole(.off)
        joined = nil
        setListeningPaused(false)
    }

    func dismissAlerts() {
        if !alerts.isEmpty { hasUnseenShares = true }
        alerts = []
    }

    func panelOpened() {
        hasUnseenShares = false
        alerts = []
        discovery.refresh()
    }

    func setVolume(_ volume: Float) {
        session.player.volume = min(max(volume, 0), 1)
    }

    /// Listening to a stereo pair: plays `channel` here; the device that had it takes this one's
    /// place. The sharer's stream already carries both channels, so this costs it nothing.
    func requestChannel(_ channel: ServerState.Channel) {
        switch channel {
        case .left: session.requestChannel(.left)
        case .right: session.requestChannel(.right)
        case .full: break
        }
    }

    /// Silences (or brings back) this device while it stays in the group and in sync, so playing
    /// again picks up exactly where the share is. A listener can't pause the sharer.
    func setListeningPaused(_ paused: Bool) {
        guard paused != isListeningPaused else { return }
        isListeningPaused = paused
        session.player.isMuted = paused
    }

    // MARK: - Sharing

    func select(_ source: ShareSource) {
        guard !isSharing else { return }
        shareSource = source
    }

    #if os(macOS)
    /// Asks for an audio file to share. The server opens it, so the server asks.
    func chooseFile() {
        if let openPanel {
            openPanel.makeKeyAndOrderFront(nil)
            NSApp.activate()
            return
        }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.audio]
        panel.allowsMultipleSelection = false
        panel.message = "Choose an audio file to share"
        openPanel = panel
        NSApp.activate()
        panel.begin { [weak self] response in
            guard let self else { return }
            openPanel = nil
            if response == .OK, let url = panel.url { select(.file(url)) }
        }
    }
    #else
    /// Shares a file picked from Files. Picked files are only readable while their security
    /// scope is open, and playing reads the file again later, so it's copied in first.
    func importFile(_ url: URL) throws {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let manager = FileManager.default
        let folder = manager.temporaryDirectory.appending(path: "Shared Audio", directoryHint: .isDirectory)
        // Only the file being shared is kept.
        try? manager.removeItem(at: folder)
        try manager.createDirectory(at: folder, withIntermediateDirectories: true)
        let copy = folder.appending(path: url.lastPathComponent)
        try manager.copyItem(at: url, to: copy)
        select(.file(copy))
    }
    #endif

    func startSharing() {
        guard let shareSource else { return }
        if mode != .share { setMode(.share) }
        session.setRole(.leader)
        switch shareSource {
        case let .application(application):
            #if os(macOS)
            session.selectAudioApplication(application)
            session.playSelectedSource()
            #else
            _ = application
            #endif
        case let .file(url):
            playsWhenLoaded = true
            session.loadTrack(from: url)
        }
        isSharing = true
        pausedPosition = nil
        announcedInfo = nil
    }

    func stopSharing() {
        guard isSharing else { return }
        isSharing = false
        playsWhenLoaded = false
        pausedPosition = nil
        announcedInfo = nil
        discovery.stop()
        session.setRole(.off)
    }

    func setSpeakerLayout(_ layout: SpeakerLayout) {
        session.setSpeakerLayout(layout)
    }

    /// The file being shared, once loaded.
    var sharedFile: MediaFile? {
        guard isSharing, shareSource?.isFile == true else { return nil }
        return session.selectedTrack
    }

    /// Whether the shared file is playing on the group (not paused, finished or still loading).
    var isFilePlaying: Bool {
        guard let file = sharedFile else { return false }
        return session.player.playback.source == file.id
    }

    /// Where the shared file is now, in seconds (its paused position while paused).
    var filePosition: Double {
        guard let file = sharedFile else { return 0 }
        if let pausedPosition { return Double(pausedPosition) / 1e9 }
        let playback = session.player.playback
        guard playback.source == file.id else { return 0 }
        // The leader's own clock is the group's timeline.
        // Before a scheduled start, it's where it will start.
        let position = max(HostClock.now() - playback.startLeaderTime, 0) + playback.startMediaTime
        return min(max(Double(position) / 1e9, 0), file.duration)
    }

    /// Pauses the shared file on every device, remembering where.
    func pauseSharing() {
        guard isFilePlaying else { return }
        pausedPosition = Int64(filePosition * 1e9)
        session.stopPlayback()
    }

    /// Plays the shared file again on every device: from where it was paused, or from the top
    /// once it has finished.
    func resumeSharing() {
        guard let file = sharedFile, !isFilePlaying else { return }
        let position = pausedPosition ?? 0
        pausedPosition = nil
        session.play(source: file.id, from: position)
    }

    /// Moves the shared file to `seconds` on every device; a paused file stays paused there.
    func seekSharing(to seconds: Double) {
        guard let file = sharedFile else { return }
        let position = Int64(min(max(seconds, 0), file.duration) * 1e9)
        if isFilePlaying {
            session.play(source: file.id, from: position)
        } else {
            pausedPosition = position
        }
    }

    // MARK: - State

    /// Everything the app shows. Reading it registers with observation, so a change to any of
    /// it can be pushed to the app.
    var state: ServerState {
        var state = ServerState()
        #if os(macOS)
        state.build = .current
        #endif
        state.mode = mode
        state.deviceName = deviceName
        state.nearby = nearby
        state.alerts = alerts
        state.hasUnseenShares = hasUnseenShares
        state.discoveryError = discoveryError
        state.isLocalNetworkDenied = session.isLocalNetworkDenied

        state.joined = joined
        if joined != nil {
            state.isPlaying = session.player.playback.isPlaying()
            state.channel = ServerState.Channel(session.player.channel)
            state.joinedLayout = ServerState.Layout(session.remoteLayout)
            state.syncStatus = syncStatus
        }
        state.volume = session.player.volume

        state.shareSource = shareSource
        state.isSharing = isSharing
        if isSharing {
            state.shareProblem = shareProblem
            state.listeners = listeners.map { member in
                let channel = session.activeChannels[member.id].map(ServerState.Channel.init)
                return ShareListener(id: member.id, name: member.name, channel: channel == .full ? nil : channel)
            }
        }
        state.layout = ServerState.Layout(session.speakerLayout)
        #if os(macOS)
        state.applications = catalog.applications
        state.applicationsError = catalog.error
        #endif
        return state
    }

    private var syncStatus: SyncStatus {
        switch session.followerState {
        case .syncing: .syncing
        case .reconnecting(ClockSyncFollower.localNetworkDenied): .localNetworkDenied
        case .reconnecting: .reconnecting
        case let .failed(message): .failed(message)
        case .connecting, .stopped, nil: .connecting
        }
    }

    private var shareProblem: String? {
        #if os(macOS)
        if let error = session.applicationAudio.error { return error }
        #endif
        if session.isLoadingTrack || playsWhenLoaded { return "Loading…" }
        #if os(macOS)
        if session.applicationAudio.isWaitingForAudio { return "Waiting for audio…" }
        #endif
        return nil
    }

    // MARK: - Polling

    private func poll() {
        let status = discovery.status()
        let error = status.error.isEmpty ? nil : String(status.error)
        if error != discoveryError { discoveryError = error }

        if isSharing { updateAnnouncement() }
        guard mode == .listen else { return }

        let current = discovery.generation()
        if current != generation {
            generation = current
            readShares()
        }
        // The share we joined went away and its leader no longer answers: we are done.
        if let joined, !nearby.contains(where: { $0.id == joined.id }), !isSyncing {
            leave()
        }
    }

    private var isSyncing: Bool {
        if case .syncing = session.followerState { return true }
        return false
    }

    private func readShares() {
        var shares: [NearbyShare] = []
        for share in discovery.shares() {
            shares.append(NearbyShare(
                id: share.share,
                device: share.device,
                name: String(share.info.name),
                source: String(share.info.source),
                sourceKind: ShareKind(share.info.sourceKind),
                deviceKind: ShareDeviceKind(share.info.deviceKind),
                model: String(share.info.model),
                listenerCount: Int(share.info.listenerCount),
                host: String(share.address.host()),
                port: share.info.clockPort,
                firstSeen: .now))
        }
        // Keep when each was first seen.
        for index in shares.indices {
            if let known = nearby.first(where: { $0.id == shares[index].id }) {
                shares[index].firstSeen = known.firstSeen
            }
        }
        nearby = shares
        if let joined, let current = shares.first(where: { $0.id == joined.id }) {
            self.joined = current
        }

        let fresh = shares.filter { !seenShares.contains($0.id) && $0.id != joined?.id }
        seenShares.formUnion(shares.map(\.id))
        alerts = alerts.compactMap { alert in shares.first { $0.id == alert.id } }
        guard !fresh.isEmpty, settings.alertBehavior == .showAlert else {
            if !fresh.isEmpty { hasUnseenShares = true }
            return
        }
        alerts.insert(contentsOf: fresh, at: 0)
        onAlert?()
    }

    private func updateAnnouncement() {
        if playsWhenLoaded, !session.isLoadingTrack {
            // Loading ended: play what loaded, or stop waiting if the file couldn't be read.
            playsWhenLoaded = false
            if session.selectedTrack != nil { session.playSelectedSource() }
        }
        guard let port = session.leaderPort else { return }
        var info = asyncmusic.ShareInfo()
        info.name = std.string(settings.deviceName)
        info.source = std.string(shareSource?.name ?? "")
        switch shareSource {
        case .application: info.sourceKind = .application
        case .file: info.sourceKind = .file
        case nil: info.sourceKind = .unknown
        }
        info.deviceKind = ThisDevice.kind
        info.model = std.string(ThisDevice.model)
        info.clockPort = port
        info.listenerCount = UInt16(clamping: listeners.count)
        if let announcedInfo {
            if announcedInfo != info {
                discovery.update(info)
                self.announcedInfo = info
            }
        } else {
            discovery.share(info)
            announcedInfo = info
        }
    }
}

extension ServerState.Layout {
    nonisolated init(_ layout: SpeakerLayout) {
        switch layout {
        case .stereoPair: self = .stereoPair
        default: self = .everywhere
        }
    }

    nonisolated var speakerLayout: SpeakerLayout {
        switch self {
        case .everywhere: .everywhere
        case .stereoPair: .stereoPair
        }
    }
}

extension ServerState.Channel {
    nonisolated init(_ channel: SpeakerChannel) {
        switch channel {
        case .left: self = .left
        case .right: self = .right
        default: self = .full
        }
    }
}

private extension ShareKind {
    init(_ kind: asyncmusic.ShareSourceKind) {
        switch kind {
        case .application: self = .application
        case .file: self = .file
        case .system: self = .system
        default: self = .unknown
        }
    }
}

private extension ShareDeviceKind {
    init(_ kind: asyncmusic.ShareDeviceKind) {
        switch kind {
        case .laptop: self = .laptop
        case .desktop: self = .desktop
        case .phone: self = .phone
        case .tablet: self = .tablet
        default: self = .unknown
        }
    }
}

/// What this device tells listeners about itself, so they can picture it.
private enum ThisDevice {
    /// The model identifier, e.g. "Mac16,10" or "iPhone17,1", which a Mac turns into a picture
    /// of the exact model.
    static let model: String = {
        #if targetEnvironment(simulator)
        if let simulated = ProcessInfo.processInfo.environment["SIMULATOR_MODEL_IDENTIFIER"] { return simulated }
        #endif
        // A Mac's model is hw.model; iOS keeps it in hw.machine.
        #if os(macOS)
        let name = "hw.model"
        #else
        let name = "hw.machine"
        #endif
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "" }
        var bytes = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &bytes, &size, nil, 0) == 0 else { return "" }
        return String(cString: bytes)
    }()

    static var kind: asyncmusic.ShareDeviceKind {
        #if os(macOS)
        isLaptop ? .laptop : .desktop
        #elseif os(iOS)
        MainActor.assumeIsolated { UIDevice.current.userInterfaceIdiom == .pad ? .tablet : .phone }
        #else
        .unknown
        #endif
    }

    #if os(macOS)
    /// Whether it runs on an internal battery: a MacBook.
    static let isLaptop: Bool = {
        guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let sources = IOPSCopyPowerSourcesList(info)?.takeRetainedValue() as? [CFTypeRef] else { return false }
        return sources.contains { source in
            let description = IOPSGetPowerSourceDescription(info, source)?.takeUnretainedValue() as? [String: Any]
            return description?[kIOPSTypeKey] as? String == kIOPSInternalBatteryType
        }
    }()
    #endif
}

extension asyncmusic.ShareInfo: @retroactive Equatable {}
