//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#if os(macOS)
import AppKit
import Observation
import ServiceManagement
import os

/// The menu bar app's line to AsyncMusic Server, which does the listening, playing and sharing.
///
/// The server runs as its own launchd job (`ServiceLauncher`), so it keeps running after this app
/// quits, until the user logs out. Launching the app starts it unless the user turned the service
/// off, which sticks until they turn it on again. Separately, a login item can start it at login.
/// The app mirrors the server's state and sends it what the user does.
@Observable
final class ServiceClient {
    enum Status: Equatable {
        /// On, and waiting to hear from the server.
        case starting
        case running
        /// The user turned the service off.
        case off
        case failed(String)
    }

    private(set) var status = Status.starting
    /// The server's state, as last heard.
    private(set) var state = ServerState()
    let settings = AppSettings.shared

    /// Whether the login item that starts the server at login is on.
    private(set) var startsAtLogin = false
    /// The login item is registered, but turned off in System Settings › Login Items.
    private(set) var loginItemNeedsApproval = false
    private(set) var loginItemError: String?

    @ObservationIgnored private let loginItem = SMAppService.agent(plistName: AsyncMusicService.loginAgentPlistName)
    @ObservationIgnored private var connection: NSXPCConnection?
    @ObservationIgnored private var statusItemFrame: CGRect?
    @ObservationIgnored private var isPanelVisible = false
    @ObservationIgnored private var reconnectTask: Task<Void, Never>?
    /// Connections that ended before the server said anything, in a row.
    @ObservationIgnored private var failedConnections = 0
    /// Set once the app has replaced an outdated server, so a server that still looks different
    /// can't make it loop.
    @ObservationIgnored private var didReplaceServer = false
    private var pendingVolume: (value: Float, date: Date)?
    @ObservationIgnored private let log = Logger(subsystem: AsyncMusicService.appBundleIdentifier, category: "Service")

    /// Whether the user wants the service on.
    var isEnabled: Bool { !UserDefaults.standard.bool(forKey: AsyncMusicService.disabledKey) }

    var isRunning: Bool { status == .running }

    private var serverExecutable: URL { ServiceLauncher.serverExecutable(inApp: Bundle.main.bundleURL) }

    func start() {
        updateLoginItemStatus()
        observeSettings()
        if isEnabled {
            launch()
        } else {
            status = .off
        }
    }

    func setEnabled(_ enabled: Bool) {
        UserDefaults.standard.set(!enabled, forKey: AsyncMusicService.disabledKey)
        if enabled {
            launch()
        } else {
            disconnect()
            status = .off
            state = ServerState()
            Task.detached { [log] in
                do {
                    try ServiceLauncher.stop()
                } catch {
                    log.error("\(error.localizedDescription, privacy: .public)")
                }
            }
        }
    }

    // MARK: - Start at login

    func setStartsAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try loginItem.register()
            } else {
                try loginItem.unregister()
            }
            loginItemError = nil
        } catch {
            loginItemError = error.localizedDescription
        }
        updateLoginItemStatus()
    }

    /// For a login item turned off in System Settings, which only the user can turn back on there.
    func openLoginItemsSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    private func updateLoginItemStatus() {
        let status = loginItem.status
        startsAtLogin = status == .enabled || status == .requiresApproval
        loginItemNeedsApproval = status == .requiresApproval
    }

    // MARK: - What the app tells the server

    func setStatusItemFrame(_ frame: CGRect?) {
        guard frame != statusItemFrame else { return }
        statusItemFrame = frame
        send(.setStatusItemFrame(frame))
    }

    func setPanelVisible(_ visible: Bool) {
        isPanelVisible = visible
        send(.setPanelVisible(visible))
        if visible {
            // The user may have changed Login Items while the app was in the background.
            updateLoginItemStatus()
        }
    }

    // MARK: - What the user does

    func setMode(_ mode: ShareMode) {
        state.mode = mode
        send(.setMode(mode))
    }

    func join(_ share: NearbyShare) {
        send(.join(share.id))
    }

    func leave() {
        send(.leave)
    }

    func requestChannel(_ channel: ServerState.Channel) {
        send(.requestChannel(channel))
    }

    /// The volume the slider shows: the one just set, until the server confirms it.
    var volume: Float {
        if let pendingVolume, Date.now.timeIntervalSince(pendingVolume.date) < 1 { return pendingVolume.value }
        return state.volume
    }

    func setVolume(_ volume: Float) {
        pendingVolume = (volume, .now)
        send(.setVolume(volume))
    }

    func refreshApplications() {
        send(.refreshApplications)
    }

    func select(_ source: ShareSource) {
        state.shareSource = source
        send(.selectSource(source))
    }

    func chooseFile() {
        // The server shows the open panel; let it come to the front.
        NSApp.yieldActivation(toApplicationWithBundleIdentifier: AsyncMusicService.serverBundleIdentifier)
        send(.chooseFile)
    }

    func startSharing() {
        send(.startSharing)
    }

    func stopSharing() {
        send(.stopSharing)
    }

    func setLayout(_ layout: ServerState.Layout) {
        state.layout = layout
        send(.setLayout(layout))
    }

    func openDiagnostics() {
        NSApp.yieldActivation(toApplicationWithBundleIdentifier: AsyncMusicService.serverBundleIdentifier)
        send(.openDiagnostics)
    }

    // MARK: - Starting

    /// Loads the server's job unless it is loaded, then connects.
    private func launch() {
        status = .starting
        let executable = serverExecutable
        Task {
            do {
                try await Task.detached { try ServiceLauncher.start(executable: executable) }.value
                guard isEnabled else { return }
                connect()
            } catch {
                log.error("\(error.localizedDescription, privacy: .public)")
                status = .failed(error.localizedDescription)
            }
        }
    }

    /// Unloads whatever server is running and starts this app's.
    private func relaunch() {
        disconnect()
        status = .starting
        let executable = serverExecutable
        Task {
            do {
                try await Task.detached {
                    try ServiceLauncher.stop()
                    try ServiceLauncher.start(executable: executable)
                }.value
                guard isEnabled else { return }
                connect()
            } catch {
                log.error("\(error.localizedDescription, privacy: .public)")
                status = .failed(error.localizedDescription)
            }
        }
    }

    // MARK: - Connection

    private func connect() {
        guard connection == nil, isEnabled else { return }
        let connection = NSXPCConnection(machServiceName: AsyncMusicService.machServiceName)
        connection.setCodeSigningRequirement(AsyncMusicService.codeSigningRequirement(identifier: AsyncMusicService.serverBundleIdentifier))
        connection.remoteObjectInterface = NSXPCInterface(with: ServerXPC.self)
        connection.exportedInterface = NSXPCInterface(with: ClientXPC.self)
        connection.exportedObject = XPCMessageReceiver { [weak self] data in
            self?.receive(data)
        }
        nonisolated(unsafe) let unsafeConnection = connection
        connection.interruptionHandler = { [weak self] in
            DispatchQueue.main.async { self?.interrupted(unsafeConnection) }
        }
        connection.invalidationHandler = { [weak self] in
            DispatchQueue.main.async { self?.invalidated(unsafeConnection) }
        }
        connection.resume()
        self.connection = connection
        introduce()
    }

    private func disconnect() {
        reconnectTask?.cancel()
        reconnectTask = nil
        guard let connection else { return }
        self.connection = nil
        connection.invalidate()
    }

    /// Tells the server, new or restarted, what it needs to know about the app.
    private func introduce() {
        send(.updateSettings(settings.service))
        send(.setStatusItemFrame(statusItemFrame))
        send(.setPanelVisible(isPanelVisible))
    }

    /// The server quit, crashed or was killed. launchd doesn't restart it on its own, but its job is
    /// still loaded, so the next message to its Mach service starts it again and reaches the new one.
    private func interrupted(_ connection: NSXPCConnection) {
        guard connection === self.connection else { return }
        log.info("The server went away")
        if status == .running { status = .starting }
        scheduleReconnect { [weak self] in
            guard let self, self.connection === connection else { return }
            introduce()
        }
    }

    /// No server to talk to: its job was unloaded, or it can't start. Load it again; if the
    /// connection keeps failing although the job is loaded, replace the job (the copy of the app
    /// it ran from may be gone).
    private func invalidated(_ connection: NSXPCConnection) {
        guard connection === self.connection else { return }
        log.info("The connection to the server ended")
        self.connection = nil
        guard isEnabled else { return }
        if status == .running { status = .starting }
        failedConnections += 1
        scheduleReconnect { [weak self] in
            guard let self else { return }
            if failedConnections >= 3 {
                failedConnections = 0
                relaunch()
            } else {
                launch()
            }
        }
    }

    private func scheduleReconnect(_ action: @escaping () -> Void) {
        reconnectTask?.cancel()
        reconnectTask = Task {
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled else { return }
            action()
        }
    }

    private func receive(_ data: Data) {
        guard connection != nil else { return }
        failedConnections = 0
        // Which build sent it comes first: a server of another build may send a state this app
        // can't read, and it is replaced either way.
        let build = (try? JSONDecoder().decode(BuildOnly.self, from: data))?.build
        if replaceServerIfOutdated(build) { return }
        do {
            let state = try JSONDecoder().decode(ServerState.self, from: data)
            if let pendingVolume, abs(pendingVolume.value - state.volume) < 0.001 { self.pendingVolume = nil }
            if state != self.state { self.state = state }
            if status != .running { status = .running }
        } catch {
            log.error("Unreadable state: \(error.localizedDescription, privacy: .public)")
        }
    }

    /// Just the build of a `ServerState`, which stays readable across builds.
    private struct BuildOnly: Decodable {
        var build: ServerBuild?
    }

    /// Replaces a server started by another copy of the app, or by an older build of this one
    /// (after an update, or a rebuild while developing), with this app's. Returns whether it did.
    private func replaceServerIfOutdated(_ build: ServerBuild?) -> Bool {
        guard !didReplaceServer else { return false }
        let expected = ServerBuild(serverBundle: serverExecutable.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent())
        guard build != expected else { return false }
        if build?.bundlePath != expected.bundlePath {
            log.info("The server runs from \(build?.bundlePath ?? "an older build", privacy: .public); replacing it with this app’s")
        } else {
            log.info("The server is an older build; restarting it")
        }
        didReplaceServer = true
        relaunch()
        return true
    }

    private func send(_ command: ServerCommand) {
        guard let connection else { return }
        do {
            let data = try JSONEncoder().encode(command)
            let proxy = connection.remoteObjectProxyWithErrorHandler { [log] error in
                log.error("Couldn't reach the server: \(error.localizedDescription)")
            } as? ServerXPC
            proxy?.send(data)
        } catch {
            log.error("Couldn't encode \(String(describing: command)): \(error.localizedDescription)")
        }
    }

    private func observeSettings() {
        let settings = withObservationTracking {
            self.settings.service
        } onChange: { [weak self] in
            DispatchQueue.main.async { self?.observeSettings() }
        }
        send(.updateSettings(settings))
    }
}
#endif
