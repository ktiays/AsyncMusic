//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import Foundation

// What AsyncMusic Server and the menu bar app say to each other. The app sends commands; the
// server answers every change with its whole state, which is small enough to send each time.

/// Whether this Mac listens for shares or shares its own audio. A Mac does one or the other.
nonisolated enum ShareMode: String, CaseIterable, Identifiable, Codable, Sendable {
    case listen = "Listen"
    case share = "Share"

    var id: Self { self }
}

/// What kind of audio a share plays.
nonisolated enum ShareKind: String, Codable, Sendable {
    case application
    case file
    case system
    case unknown
}

/// What kind of device shares, for when its model has no picture.
nonisolated enum ShareDeviceKind: String, Codable, Sendable {
    case laptop
    case desktop
    case phone
    case tablet
    case unknown
}

/// A share heard on the network.
nonisolated struct NearbyShare: Identifiable, Equatable, Codable, Sendable {
    /// The share's ID: a new one each time the device starts sharing.
    let id: UInt64
    let device: UInt64
    var name: String
    var source: String
    var sourceKind: ShareKind
    var deviceKind: ShareDeviceKind
    /// The sharing device's model identifier, e.g. "Mac16,10"; empty when it didn't say.
    var model: String
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
        case .unknown: "Audio"
        }
    }
}

/// What this Mac shares.
nonisolated enum ShareSource: Equatable, Codable, Sendable {
    case application(AudioApplication)
    case file(URL)

    var name: String {
        switch self {
        case let .application(application): application.name
        case let .file(url): url.deletingPathExtension().lastPathComponent
        }
    }

    var isFile: Bool {
        if case .file = self { true } else { false }
    }
}

/// How a joined share's clock sync is going.
nonisolated enum SyncStatus: Equatable, Codable, Sendable {
    case connecting
    case syncing
    case reconnecting
    case localNetworkDenied
    case failed(String)
}

/// A device listening to this Mac's share.
nonisolated struct ShareListener: Identifiable, Equatable, Codable, Sendable {
    var id: UInt64
    var name: String
    /// The channel it plays, when the layout gives it one.
    var channel: ServerState.Channel?
}

/// The preferences the server acts on. The app owns them and sends them on every change; the
/// server keeps the last ones so it behaves the same after a restart without the app.
nonisolated struct ServiceSettings: Equatable, Codable, Sendable {
    enum AlertBehavior: String, CaseIterable, Identifiable, Codable, Sendable {
        case showAlert
        case onlyList

        var id: Self { self }
        var title: String {
            switch self {
            case .showAlert: "Show Alert"
            case .onlyList: "Only List"
            }
        }
    }

    /// The name other devices see. Empty means the Mac's own name.
    var customDeviceName = ""
    var alertBehavior = AlertBehavior.showAlert

    var deviceName: String {
        let trimmed = customDeviceName.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? Self.systemDeviceName : trimmed
    }

    static var systemDeviceName: String { Host.current().localizedName ?? "Mac" }
}

/// Which copy and build of AsyncMusic Server is running. The server outlives the app that started
/// it, so after the app is updated or rebuilt, or another copy of it starts, the server running
/// can be an old build or another copy's; the app checks this against the server it carries.
nonisolated struct ServerBuild: Equatable, Codable, Sendable {
    var bundlePath: String
    /// When the server's code last changed on disk.
    var stamp: Date

    /// The running server's build, as it was when it launched.
    static let current = ServerBuild(serverBundle: Bundle.main.bundleURL)

    init(serverBundle url: URL) {
        let url = url.resolvingSymlinksInPath()
        bundlePath = url.path
        let contents = url.appending(path: "Contents")
        let manager = FileManager.default
        var files = [contents.appending(path: "_CodeSignature/CodeResources"), contents.appending(path: "Info.plist")]
        let executables = contents.appending(path: "MacOS")
        files += (try? manager.contentsOfDirectory(at: executables, includingPropertiesForKeys: nil)) ?? []
        stamp = files.compactMap { try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate }.max() ?? .distantPast
    }
}

/// Everything the app shows, as the server sees it.
nonisolated struct ServerState: Equatable, Codable, Sendable {
    var build: ServerBuild?
    var mode = ShareMode.listen
    var deviceName = ""
    var nearby: [NearbyShare] = []
    /// New shares waiting for an answer, newest first.
    var alerts: [NearbyShare] = []
    /// Set when an alert went unanswered; cleared when the panel opens.
    var hasUnseenShares = false
    var discoveryError: String?
    var isLocalNetworkDenied = false

    // Listening
    var joined: NearbyShare?
    var isPlaying = false
    var channel = Channel.full
    var volume: Float = 1
    var syncStatus: SyncStatus?

    // Sharing
    var shareSource: ShareSource?
    var isSharing = false
    /// Why sharing isn't playing yet, or nil when it is.
    var shareProblem: String?
    var layout = Layout.everywhere
    var listeners: [ShareListener] = []
    var applications: [AudioApplication] = []
    var applicationsError: String?

    var nearbyOthers: [NearbyShare] { nearby.filter { $0.id != joined?.id } }

    /// Whether switching to `mode` would cut a connection, so the user should confirm.
    func switchInterrupts(to mode: ShareMode) -> Bool {
        mode != self.mode && (joined != nil || isSharing)
    }

    /// How a share spreads its sound over the devices listening (the core's `SpeakerLayout`).
    enum Layout: String, CaseIterable, Identifiable, Codable, Sendable {
        case everywhere
        case stereoPair

        var id: Self { self }
        var title: String {
            switch self {
            case .everywhere: "Everywhere"
            case .stereoPair: "Stereo Pair"
            }
        }
    }

    /// What a device plays of the share's sound (the core's `SpeakerChannel`).
    enum Channel: String, Codable, Sendable {
        case full
        case left
        case right

        var title: String {
            switch self {
            case .full: "Full Mix"
            case .left: "Left"
            case .right: "Right"
            }
        }

        /// One or two letters for a speaker badge.
        var badge: String {
            switch self {
            case .full: "LR"
            case .left: "L"
            case .right: "R"
            }
        }
    }
}

/// What the app asks the server to do.
nonisolated enum ServerCommand: Codable, Sendable {
    case updateSettings(ServiceSettings)
    /// Where the app's menu bar icon is, in screen coordinates, so alerts can point at it; nil
    /// when the app shows no icon.
    case setStatusItemFrame(CGRect?)
    case setPanelVisible(Bool)

    case setMode(ShareMode)
    case join(UInt64)
    case leave
    case dismissAlerts
    case setVolume(Float)

    case refreshApplications
    case selectSource(ShareSource)
    /// Shows an open panel from the server, which is the process that reads the file.
    case chooseFile
    case startSharing
    case stopSharing
    case setLayout(ServerState.Layout)

    case openDiagnostics
}
#endif
