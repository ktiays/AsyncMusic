//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import AsyncMusicCore
import Network
import Observation

typealias ShareDiscovery = asyncmusic.ShareDiscovery
typealias ShareSourceKind = asyncmusic.ShareSourceKind

/// The menu bar app's model: whether this Mac listens for shares or shares its own audio, the
/// shares heard nearby and the alerts for new ones, and the group session doing the playing.
///
/// Listening and sharing exclude each other, as `ShareDiscovery` does on the network. Listening
/// is the default, so alerts arrive without opening anything. Joining a share follows the
/// sharer's clock leader at the address it announced; sharing leads, and announces the leader's
/// port once it is listening.
@Observable
final class ShareCenter {
    enum Mode: String, CaseIterable, Identifiable {
        case listen = "Listen"
        case share = "Share"

        var id: Self { self }
    }

    /// A share heard on the network.
    struct NearbyShare: Identifiable, Equatable {
        /// The share's ID: a new one each time the device starts sharing.
        let id: UInt64
        let device: UInt64
        var name: String
        var source: String
        var sourceKind: ShareSourceKind
        var listenerCount: Int
        var host: String
        var port: UInt16
        var firstSeen: Date

        var summary: String {
            let source = self.source.isEmpty ? kindName : self.source
            switch listenerCount {
            case 0: return source
            case 1: return "\(source) · 1 listening"
            default: return "\(source) · \(listenerCount) listening"
            }
        }

        private var kindName: String {
            switch sourceKind {
            case .application: "App audio"
            case .file: "Audio file"
            case .system: "Mac audio"
            default: "Audio"
            }
        }
    }

    /// What this Mac shares.
    enum ShareSource: Equatable {
        case application(AudioApplication)
        case file(URL)

        var name: String {
            switch self {
            case let .application(application): application.name
            case let .file(url): url.deletingPathExtension().lastPathComponent
            }
        }
    }

    let session = ClockSyncSession()
    let settings = AppSettings.shared

    private(set) var mode = Mode.listen
    private(set) var nearby: [NearbyShare] = []
    /// New shares waiting for an answer, newest first.
    private(set) var alerts: [NearbyShare] = []
    /// Set when an alert went unanswered; cleared when the panel opens.
    private(set) var hasUnseenShares = false
    private(set) var joined: NearbyShare?

    var shareSource: ShareSource?
    private(set) var isSharing = false
    private(set) var discoveryError: String?

    /// Called when a new alert arrives, for the window that shows it.
    @ObservationIgnored var onAlert: (() -> Void)?

    @ObservationIgnored private let discovery: ShareDiscovery
    @ObservationIgnored private var generation: UInt64 = 0
    @ObservationIgnored private var seenShares: Set<UInt64> = []
    @ObservationIgnored private var announcedInfo: asyncmusic.ShareInfo?
    @ObservationIgnored private var playsWhenLoaded = false
    @ObservationIgnored private var pollTask: Task<Void, Never>?

    init() {
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

    var nearbyOthers: [NearbyShare] { nearby.filter { $0.id != joined?.id } }

    /// Whether switching to `mode` would cut a connection, so the user should confirm.
    func switchInterrupts(to mode: Mode) -> Bool {
        mode != self.mode && (joined != nil || isSharing)
    }

    func setMode(_ mode: Mode) {
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

    func dismissAlert(_ share: NearbyShare) {
        alerts.removeAll { $0.id == share.id }
        hasUnseenShares = true
    }

    func panelOpened() {
        hasUnseenShares = false
        alerts = []
        discovery.refresh()
    }

    // MARK: - Sharing

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

    var listeners: [ClockSyncLeader.Member] { session.members.filter(\.isActive) }

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
                sourceKind: share.info.sourceKind,
                listenerCount: Int(share.info.listenerCount),
                host: String(share.address.host()),
                port: share.info.clockPort,
                firstSeen: .now))
        }
        // Keep when each was first seen in this app.
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

extension asyncmusic.ShareInfo: @retroactive Equatable {}
#endif
