//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AppKit
import AsyncMusicCore
import Network
import Observation
import UniformTypeIdentifiers

typealias ShareDiscovery = asyncmusic.ShareDiscovery

/// The server's model: whether this Mac listens for shares or shares its own audio, the shares
/// heard nearby and the alerts for new ones, and the group session doing the playing.
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
    let catalog = AudioApplicationCatalog()

    /// Called when a new alert arrives, for the window that shows it.
    @ObservationIgnored var onAlert: (() -> Void)?

    @ObservationIgnored private let discovery: ShareDiscovery
    @ObservationIgnored private var generation: UInt64 = 0
    @ObservationIgnored private var seenShares: Set<UInt64> = []
    @ObservationIgnored private var announcedInfo: asyncmusic.ShareInfo?
    @ObservationIgnored private var playsWhenLoaded = false
    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var openPanel: NSOpenPanel?

    private static let settingsKey = "ShareCenter.settings"
    /// Where `DeviceIdentity` reads the custom name from.
    private static let deviceNameKey = "AppSettings.deviceName"

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
        defaults.set(settings.customDeviceName, forKey: Self.deviceNameKey)
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

    // MARK: - Sharing

    func select(_ source: ShareSource) {
        guard !isSharing else { return }
        shareSource = source
    }

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

    func startSharing() {
        guard let shareSource else { return }
        if mode != .share { setMode(.share) }
        session.setRole(.leader)
        switch shareSource {
        case let .application(application):
            session.selectAudioApplication(application)
            session.playSelectedSource()
        case let .file(url):
            playsWhenLoaded = true
            session.loadTrack(from: url)
        }
        isSharing = true
        announcedInfo = nil
    }

    func stopSharing() {
        guard isSharing else { return }
        isSharing = false
        playsWhenLoaded = false
        announcedInfo = nil
        discovery.stop()
        session.setRole(.off)
    }

    func setSpeakerLayout(_ layout: SpeakerLayout) {
        session.setSpeakerLayout(layout)
    }

    // MARK: - State

    /// Everything the app shows. Reading it registers with observation, so a change to any of
    /// it can be pushed to the app.
    var state: ServerState {
        var state = ServerState()
        state.build = .current
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
        state.applications = catalog.applications
        state.applicationsError = catalog.error
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
        if let error = session.applicationAudio.error { return error }
        if session.isLoadingTrack { return "Loading…" }
        if session.applicationAudio.isWaitingForAudio { return "Waiting for audio…" }
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
        if settings.playsAlertSound { NSSound(named: "Glass")?.play() }
        onAlert?()
    }

    private func updateAnnouncement() {
        if playsWhenLoaded, session.selectedTrack != nil, !session.isLoadingTrack {
            playsWhenLoaded = false
            session.playSelectedSource()
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

extension asyncmusic.ShareInfo: @retroactive Equatable {}
